#include <sys/socket.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/ioctl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/times.h>
#include <errno.h>

#ifndef VANILLA_KERNEL
#include <linux/rlimit_noti.h>
#endif

#define MAX_PAYLOAD 10

#define N_FILES 40000

/* should be in netlink.h when compiling with correct headers */
#define NETLINK_RLIMIT_EVENTS 23

#ifndef VANILLA_KERNEL
int configure_noti(pid_t child, int noti_fd)
{
	int ret;
	struct rlimit_noti_level nlvl = {
		.subj = {
			.pid = child,
			.resource = 7 /* RLIM_NOFILE */
		},
		.value = N_FILES,
		.flags = 0,
	};

	ret = ioctl(noti_fd, RLIMIT_ADD_NOTI_LVL, &nlvl);

	printf("ioctl() result %d\n", ret);
	return ret;
}

#endif

int set_big_limit()
{
	int ret;
	struct rlimit limits = {
		.rlim_cur = 50000,
		.rlim_max = 50000,
	};

	ret  = setrlimit(RLIMIT_NOFILE, &limits);
	if (ret < 0) {
		perror("setrlimit");
		return 1;
	}

	return 0;
}
int main()
{
	int i;
	int ret;
#ifndef VANILLA_KERNEL
	struct sockaddr_nl src_addr, dest_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	struct nlmsghdr *nlh = NULL;
	struct iovec iov;
	int sock_fd;
	int child_pipe[2];
	int noti_fd;
	char buf[100];
	struct rlimit_event *ev;
	struct rlimit_event_res_changed *res_ev;


	sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_RLIMIT_EVENTS);
	if(sock_fd < 0)
		return -1;

	nlh = malloc(NLMSG_SPACE(MAX_PAYLOAD));
	if (!nlh)
		goto close_socket;

	memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
	nlh->nlmsg_len = NLMSG_SPACE(0);
	nlh->nlmsg_type = RLIMIT_GET_NOTI_FD;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.nl_family = AF_NETLINK;
	dest_addr.nl_pid = 0;
	dest_addr.nl_groups = 0;

	printf("Sending message to kernel\n");
	ret = sendto(sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
		printf("Result: %d\n", ret);
	if (ret < 0)
		goto free_nlh;

	printf("Waiting for message from kernel\n");
        /* Read message from kernel */
	ret = recvfrom(sock_fd, nlh, NLMSG_SPACE(MAX_PAYLOAD), 0,
		       (struct sockaddr*)&src_addr, &src_addr_len);
	printf("Received (%d) message payload: %d\n", ret, *((int *)NLMSG_DATA(nlh)));
	noti_fd = *((int *)NLMSG_DATA(nlh));

	printf("Forking...");
	pipe(child_pipe);

	ret = fork();
	if (ret == 0) {
#endif
		int dum;
		struct tms tms_start, tms_stop;
		char path[4096];
		
		/* child */
#ifndef VANILLA_KERNEL
		close(child_pipe[1]);
		read(child_pipe[0], &dum, sizeof(dum));
#endif

		ret = set_big_limit();
		if (ret)
			exit(ret);

		times(&tms_start);
		for (i = 0; i < N_FILES; ++i) {
			snprintf(path, sizeof(path), "/root/files/file-%d", i);
			ret = open(path, O_RDONLY);
			if (ret < 0) 
				printf("open failed %d %d\n", i, errno);
		}
		times(&tms_stop);

		printf("User: %f s\n", ((float)(tms_stop.tms_utime - tms_start.tms_utime))/sysconf(_SC_CLK_TCK));
		printf("Sys: %f s\n", ((float)(tms_stop.tms_stime - tms_start.tms_stime))/sysconf(_SC_CLK_TCK));
		
		exit(0);
		/* child dead */
#ifndef VANILLA_KERNEL
	}
	close(child_pipe[0]);

	printf("Configuring FD to get notification after 10 descriptors\n");
	configure_noti(ret, noti_fd);

	write(child_pipe[1], &ret, sizeof(ret));

	ret = read(noti_fd, buf, sizeof(buf));
	if (ret < 0) {
		printf("Read event failed %d\n", ret);
		goto free_nlh;
	}

	ev = (typeof(ev))buf;
	res_ev = (typeof(res_ev))(buf + sizeof(*ev));

	printf("Got event: %d, pid: %d, res: %d, new value %ld\n", ev->ev_type, res_ev->subj.pid, res_ev->subj.resource, res_ev->new_value);
free_nlh:
	free(nlh);
close_socket:
	close(sock_fd);
#endif
	return 0;
}
