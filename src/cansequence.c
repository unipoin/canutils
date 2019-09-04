#include <can_config.h>

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include <net/if.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_ID_DEFAULT	(2)
#define DEFAULT_DATA_BYTES (1)

extern int optind, opterr, optopt;

static int s = -1;
static bool running = true;
static bool infinite = true;
static bool sequence_init = true;
static unsigned int drop_until_quit;
static unsigned int drop_count;
static bool use_poll = false;

static unsigned int loopcount = 1;
static int verbose;

static int receive = 0;
static FILE *fh = NULL;
static int received_frames = 0;
static int sequence_missmatches = 0;
static char *filename = NULL;

static struct can_frame frame = {
	.can_dlc = 1,
};
static struct can_filter filter[] = {
	{ .can_id = CAN_ID_DEFAULT, },
};
enum {
	VERSION_OPTION = CHAR_MAX + 1,
};

static void print_usage(char *prg)
{
	fprintf(stderr, "Usage: %s [<can-interface>] [Options]\n"
		"\n"
		"cansequence sends CAN messages with a rising sequence number as payload.\n"
		"When the -r option is given, cansequence expects to receive these messages\n"
		"and prints an error message if a wrong sequence number is encountered.\n"
		"The main purpose of this program is to test the reliability of CAN links.\n"
		"\n"
		"Options:\n"
		" -e  --extended		send extended frame\n"
		" -i, --identifier=ID	CAN Identifier (default = %u)\n"
		" -r, --receive		work as receiver\n"
		"     --loop=COUNT	send message COUNT times\n"
		" -p  --poll		use poll(2) to wait for buffer space while sending\n"
		" -q  --quit <num>	quit if <num> wrong sequences are encountered\n"
		" -v, --verbose		be verbose (twice to be even more verbose\n"
		" -h  --help		this help\n"
		"     --version		print version information and exit\n"
                " -d, --datalen=LEN     specify the bytes send (default = 1)\n"
                " -f, --file=filename   file to write received frames to\n"
		, prg, CAN_ID_DEFAULT);
}

static void prepare_quit(void) {
	if (receive) {
		printf("statistic: totally received frames: %d, sequence missmatches: %d\n", received_frames, sequence_missmatches);
		if (fh != NULL)
			fprintf(fh, "statistic: totally received frames: %d, sequence missmatches: %d\n", received_frames, sequence_missmatches);
	}
	if (fh != NULL) {
		fclose(fh);
	}
}


static void sigterm(int signo)
{
	printf("received Signal: %u, will gracefully stop!\n", signo);
	running = 0;
	prepare_quit();
	exit(EXIT_SUCCESS);
}

static void do_receive()
{

	uint8_t ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
	struct iovec iov = {
		.iov_base = &frame,
	};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = &ctrlmsg,
	};
	struct cmsghdr *cmsg;
	const int dropmonitor_on = 1;
	unsigned int seq_wrap = 0;
	uint8_t sequence = 0;
	ssize_t nbytes;

	if (filename != NULL && receive == 1) {
		printf("open %s to dump received frames.\n", filename);
		fh = fopen("fileopen","w+");
		if(fh == NULL) {
			perror("open file for writing");
			exit(EXIT_FAILURE);
		}
	}

	if (setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL,
		       &dropmonitor_on, sizeof(dropmonitor_on)) < 0) {
		perror("setsockopt() SO_RXQ_OVFL not supported by your Linux Kernel");
	}

	/* enable recv. now */
	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, filter, sizeof(filter))) {
		perror("setsockopt()");
		exit(EXIT_FAILURE);
	}

	while ((infinite || loopcount--) && running) {
		msg.msg_iov[0].iov_len = sizeof(frame);
		msg.msg_controllen = sizeof(ctrlmsg);
		msg.msg_flags = 0;
		nbytes = recvmsg(s, &msg, 0);

		if (nbytes < 0) {
			perror("read()");
			exit(EXIT_FAILURE);
		}

		if (sequence_init) {
			sequence_init = 0;
			sequence = frame.data[0];
		}

		if (fh != NULL)
			fprintf(fh, "received frame. sequence number: %d\n", frame.data[0]);

		if (verbose > 1)
			printf("received frame. sequence number: %d\n", frame.data[0]);
		
		received_frames++;

		if (frame.data[0] != sequence) {
			uint32_t overflows = 0;

			drop_count++;
			sequence_missmatches++;

			for (cmsg = CMSG_FIRSTHDR(&msg);
			     cmsg && (cmsg->cmsg_level == SOL_SOCKET);
			     cmsg = CMSG_NXTHDR(&msg,cmsg)) {
				if (cmsg->cmsg_type == SO_RXQ_OVFL) {
					memcpy(&overflows, CMSG_DATA(cmsg), sizeof(overflows));
					break;
				}
			}

			fprintf(stderr, "[%d] received wrong sequence count. expected: %d, got: %d, socket overflows: %u\n", drop_count, sequence, frame.data[0], overflows);
			if (fh != NULL)
				fprintf(stderr, "[%d] received wrong sequence count. expected: %d, got: %d, socket overflows: %u\n", drop_count, sequence, frame.data[0], overflows);
				

			if (drop_count == drop_until_quit)
				exit(EXIT_FAILURE);

			sequence = frame.data[0];
		}

		sequence++;
		if (fh != NULL && !sequence)
			fprintf(fh,"sequence wrap around (%d)\n", seq_wrap);

		if (verbose && !sequence)
			printf("sequence wrap around (%d)\n", seq_wrap++);

	}
}

static void do_send()
{
	unsigned int seq_wrap = 0;
	uint8_t sequence = 0;
	uint32_t send_frames = 0;
	struct timeval tv;
	gettimeofday(&tv,NULL);
	uint32_t timestamp_start_sending_us = 1000000 * tv.tv_sec + tv.tv_usec;

	while ((infinite || loopcount--) && running) {
		ssize_t len;
		
		send_frames++;

		if (verbose > 1)
			printf("sending frame. sequence number: %d\n", sequence);

	again:
		len = write(s, &frame, sizeof(frame));
		if (len == -1) {
			switch (errno) {
			case ENOBUFS: {
				int err;
				struct pollfd fds[] = {
					{
						.fd	= s,
						.events	= POLLOUT,
					},
				};

				if (!use_poll) {
					perror("write");
					exit(EXIT_FAILURE);
				}

				err = poll(fds, 1, 1000);
				if (err == -1 && errno != -EINTR) {
					perror("poll()");
					exit(EXIT_FAILURE);
				}
			}
			case EINTR:	/* fallthrough */
				goto again;
			default:
				perror("write");
				exit(EXIT_FAILURE);
			}
		}

		(unsigned char)frame.data[0]++;
		sequence++;

		if (!(send_frames % 10000))
			printf("%d frames send\n", send_frames);

		if (verbose) {
			if (!sequence)
				printf("sequence wrap around (%d)\n", seq_wrap++);
		}
	}
	
	gettimeofday(&tv,NULL);
	uint32_t timestamp_end_sending_us = 1000000 * tv.tv_sec + tv.tv_usec;
	uint32_t time_sending = timestamp_end_sending_us - timestamp_start_sending_us;
	uint32_t time_per_frame = time_sending / send_frames;
	printf("send %u frames in %fs (takes %u us per frame)\n", send_frames, ((float) time_sending)/1000000.0f, time_per_frame);
}

int main(int argc, char **argv)
{
	struct ifreq ifr;
	struct sockaddr_can addr;
	char *interface = "can0";
	int family = PF_CAN, type = SOCK_RAW, proto = CAN_RAW;
	int nbytes = DEFAULT_DATA_BYTES;
	int extended = 0;
	int opt;

	signal(SIGINT, sigterm);
	signal(SIGTERM, sigterm);
	signal(SIGHUP, sigterm);


	struct option long_options[] = {
		{ "extended",	no_argument,		0, 'e' },
		{ "help",	no_argument,		0, 'h' },
		{ "poll",	no_argument,		0, 'p' },
		{ "quit",	optional_argument,	0, 'q' },
		{ "receive",	no_argument,		0, 'r' },
		{ "verbose",	no_argument,		0, 'v' },
		{ "version",	no_argument,		0, VERSION_OPTION},
		{ "identifier",	required_argument,	0, 'i' },
		{ "loop",	required_argument,	0, 'l' },
		{ "datalen",    required_argument,      0, 'd' },
		{ "file",       required_argument,      0, 'f' },
		{ 0,		0,			0, 0},
	};

	while ((opt = getopt_long(argc, argv, "ehpq:rvi:l:d:f:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'e':
			extended = true;
			break;

		case 'h':
			print_usage(basename(argv[0]));
			exit(EXIT_SUCCESS);
			break;

		case 'p':
			use_poll = true;
			break;

		case 'q':
			if (optarg)
				drop_until_quit = strtoul(optarg, NULL, 0);
			else
				drop_until_quit = 1;
			break;

		case 'r':
			receive = true;
			break;

		case 'v':
			verbose++;
			break;

		case VERSION_OPTION:
			printf("cansequence %s\n", VERSION);
			exit(EXIT_SUCCESS);
			break;

		case 'l':
			if (optarg) {
				loopcount = strtoul(optarg, NULL, 0);
				infinite = false;
			} else {
				infinite = true;
			}
			break;

		case 'i':
			filter->can_id = strtoul(optarg, NULL, 0);
			break;

		case 'd':
			nbytes = strtoul(optarg, NULL, 0);
			if (nbytes > 8) {
				nbytes = 8;
			}
			frame.can_dlc = nbytes;
			break;

		case 'f':
			printf("\n%s\n", optarg);
			filename = optarg;
			if (filename == NULL) {
				printf("filename not valid\n");
				exit(EXIT_FAILURE);
			}
			break;

		default:
			fprintf(stderr, "Unknown option %c\n", opt);
			break;
		}
	}

	if (argv[optind] != NULL)
		interface = argv[optind];

	if (extended) {
		filter->can_mask = CAN_EFF_MASK;
		filter->can_id  &= CAN_EFF_MASK;
		filter->can_id  |= CAN_EFF_FLAG;
	} else {
		filter->can_mask = CAN_SFF_MASK;
		filter->can_id  &= CAN_SFF_MASK;
	}
	frame.can_id = filter->can_id;

	printf("interface = %s, family = %d, type = %d, proto = %d\n",
	       interface, family, type, proto);

	s = socket(family, type, proto);
	if (s < 0) {
		perror("socket()");
		exit(EXIT_FAILURE);
	}

	addr.can_family = family;
	strncpy(ifr.ifr_name, interface, sizeof(ifr.ifr_name));
	if (ioctl(s, SIOCGIFINDEX, &ifr)) {
		perror("ioctl()");
		exit(EXIT_FAILURE);
	}
	addr.can_ifindex = ifr.ifr_ifindex;

	/* first don't recv. any msgs */
	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0)) {
		perror("setsockopt()");
		exit(EXIT_FAILURE);
	}

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind()");
		exit(EXIT_FAILURE);
	}

	if (receive)
		do_receive();
	else
		do_send();

	prepare_quit();
	exit(EXIT_SUCCESS);
}
