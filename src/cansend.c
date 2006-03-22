#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <libgen.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <can_config.h>

#include <socket-can/can.h>

extern int optind, opterr, optopt;

static void print_usage(char *prg)
{
        fprintf(stderr, "Usage: %s <can-interface> [Options] <can-msg>\n"
	                "<can-msg> can consist of up to 8 bytes given as a space separated list\n"
                        "Options:\n"
			" -i, --identifier=ID   CAN Identifier (default = 1)\n"
			" -r  --rtr             send remote request\n"
			" -e  --extended        send extended frame\n"
	                " -f, --family=FAMILY   Protocol family (default PF_CAN = %d)\n"
                        " -t, --type=TYPE       Socket type, see man 2 socket (default SOCK_RAW = %d)\n"
                        " -p, --protocol=PROTO  CAN protocol (default CAN_PROTO_RAW = %d)\n"
			" -l                    send message infinite times\n"
			"     --loop=COUNT      send message COUNT times\n"
                        " -v, --verbose         be verbose\n"
			" -h, --help            this help\n"
			"     --version         print version information and exit\n",
				prg, PF_CAN, SOCK_RAW, CAN_PROTO_RAW);
}	

enum
{
	VERSION_OPTION = CHAR_MAX + 1,
};

int main(int argc, char **argv)
{
	int family = PF_CAN, type = SOCK_RAW, proto = CAN_PROTO_RAW;
	struct sockaddr_can addr;
	int s, opt, ret, i, dlc = 0, rtr = 0, extended = 0;
	struct can_frame frame;
	int verbose = 0;
	int loopcount = 1, infinite = 0;
	struct ifreq ifr;

	struct option		long_options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "identifier", required_argument, 0, 'i'},
		{ "rtr", no_argument, 0, 'r'},
		{ "extended", no_argument, 0, 'e'},
		{ "family", required_argument, 0, 'f' },
		{ "protocol", required_argument, 0, 'p' },
		{ "type", required_argument, 0, 't' },
		{ "version", no_argument, 0, VERSION_OPTION},
		{ "verbose", no_argument, 0, 'v'},
		{ "loop", required_argument, 0, 'l'},
		{ 0, 0, 0, 0},
	};

	frame.can_id = 1;

	while ((opt = getopt_long(argc, argv, "hf:t:p:vi:lre", long_options, NULL)) != -1) {
		switch (opt) {
			case 'h':
				print_usage(basename(argv[0]));
				exit(0);

			case 'f':
				family = strtoul(optarg, NULL, 0);
				break;

			case 't':
				type = strtoul(optarg, NULL, 0);
				break;

			case 'p':
				proto = strtoul(optarg, NULL, 0);
				break;

			case 'v':
				verbose = 1;
				break;

			case 'l':
				if(optarg)
					loopcount = strtoul(optarg, NULL, 0);
				else
					infinite = 1;
				break;
			case 'i':
				frame.can_id = strtoul(optarg, NULL, 0);
				break;

			case 'r':
				rtr = 1;
				break;

			case 'e':
				extended = 1;
				break;

			case VERSION_OPTION:
				printf("cansend %s\n",VERSION);
				exit(0);

			default:
				fprintf(stderr, "Unknown option %c\n", opt);
				break;
		}
	}

	if (optind == argc) {
		print_usage(basename(argv[0]));
		exit(0);
        }
	
	if (argv[optind] == NULL) {
		fprintf(stderr, "No Interface supplied\n");
		exit(-1);
	}

	if(verbose)
		printf("interface = %s, family = %d, type = %d, proto = %d\n",
		       argv[optind], family, type, proto);
	if ((s = socket(family, type, proto)) < 0) {
		perror("socket");
		return 1;
	}

	addr.can_family = family;
	strcpy(ifr.ifr_name, argv[optind]);
	if( ioctl(s, SIOCGIFINDEX, &ifr) ) {
		perror("ioctl");
		return 1;
	}
	addr.can_ifindex = ifr.ifr_ifindex;
	addr.can_id = frame.can_id;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}


	for(i = optind + 1; i < argc; i++) {
		frame.payload.data[dlc] = strtoul(argv[i], NULL, 0);
		dlc++;
		if( dlc == 8 )
			break;
	}
	frame.can_dlc = dlc;

	if(rtr)
		frame.can_id |= CAN_FLAG_RTR;

	if(extended)
		frame.can_id |= CAN_FLAG_EXTENDED;

	if(verbose) {
		printf("id: %d ",frame.can_id);
		printf("dlc: %d\n",frame.can_dlc);
		for(i = 0; i < frame.can_dlc; i++)
			printf("0x%02x ",frame.payload.data[i]);
		printf("\n");
	}

	while (infinite || loopcount--) {
		ret = write(s, &frame, sizeof(frame));
		if( ret == -1 ) {
			perror("write");
			break;
		}
	}

	close(s);
	return 0;
}