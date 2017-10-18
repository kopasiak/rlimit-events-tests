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
#include <linux/limits.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>
#include <inttypes.h>

#include <linux/rlimit_noti.h>

#define MAX_PAYLOAD 10

#define N_FILES 40000

#ifdef ENABLE_DEBUG
int debug_enabled = 1;
#define log(msg, ...)						\
	do {							\
		if (!debug_enabled)				\
			break;					\
		fprintf(stderr, "%s %d: " msg,			\
			__FUNCTION__, __LINE__, ##__VA_ARGS__);	\
		fprintf(stderr, "\n");				\
		fflush(stderr);					\
	} while (0)
#else
#define log(...) do { ; } while (0)
#endif


/* should be in netlink.h when compiling with correct headers */
#define NETLINK_RLIMIT_EVENTS 23

#define RLIM_NOFILE 7
int test_netlink()
{
	int sock_fd;
	struct nlmsghdr *nlh = NULL;
	struct sockaddr_nl src_addr, dest_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	int noti_fd;
	int ret;

	sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_RLIMIT_EVENTS);
	if(sock_fd < 0)
		return -1;

	nlh = malloc(NLMSG_SPACE(MAX_PAYLOAD));
	if (!nlh) {
		ret = -ENOMEM;
		goto close_socket;
	}

	memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
	nlh->nlmsg_len = NLMSG_SPACE(0);
	nlh->nlmsg_type = RLIMIT_GET_NOTI_FD;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = 1;

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.nl_family = AF_NETLINK;
	dest_addr.nl_pid = 0;
	dest_addr.nl_groups = 0;

	log("Sending message to kernel");
	ret = sendto(sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
	log("Result: %d\n", ret);
	if (ret < 0)
		goto free_nlh;

	log("Waiting for message from kernel");
        /* Read message from kernel */
	ret = recvfrom(sock_fd, nlh, NLMSG_SPACE(MAX_PAYLOAD), 0,
		       (struct sockaddr*)&src_addr, &src_addr_len);
	log("Received (%d) message payload: %d", ret, *((int *)NLMSG_DATA(nlh)));
	/* Get the notification fd */
	noti_fd = *((int *)NLMSG_DATA(nlh));

	ret = fcntl(noti_fd, F_GETFD);
	if (ret < 0) {
		log("Received invalid fd from kernel: %d", ret);
		ret = -EBADFD;
		goto free_nlh;
	}

	ret = noti_fd;
free_nlh:
	free(nlh);
close_socket:
	close(sock_fd);
	return ret;
}

int test_register_noti_lvl(int noti_fd, pid_t pid, int resource, uint64_t value)
{
	struct rlimit_noti_level nlvl = {
		.subj = {
			.pid = pid,
			.resource = resource,
		},
		.value = value,
		.flags = 0,
	};
	int ret;

	log("Adding new noti level");
	ret = ioctl(noti_fd, RLIMIT_ADD_NOTI_LVL, &nlvl);

	log("ioctl() result %d\n", ret);

	return ret;
}

int test_set_limit(uint64_t level)
{
	struct rlimit limits = {
		.rlim_cur = level,
		.rlim_max = level,
	};
	int ret;

	ret  = setrlimit(RLIMIT_NOFILE, &limits);
	if (ret < 0) {
		log(" Unable to set rlimit");
		return ret;
	}

	return 0;
}

int test_open(int n_iter, const char *file_prefix)
{
	char buf[PATH_MAX];
	int ret;
	int i;

	for (i = 0; i < n_iter; ++i) {
		snprintf(buf, sizeof(buf),"%s%d", file_prefix, i);
		ret = open(buf, O_RDONLY);
		if (ret < 0)
			log("open failed %d %d\n", i, errno);
	}

	return 0;
}

int test_open_with_time(int n_iter, const char *file_prefix)
{
	struct tms tms_start, tms_stop;
	float utime, stime;
	int ret;

	times(&tms_start);
	ret = test_open(n_iter, file_prefix);
	times(&tms_stop);

	utime = ((float)(tms_stop.tms_utime - tms_start.tms_utime))/sysconf(_SC_CLK_TCK);
	stime = ((float)(tms_stop.tms_stime - tms_start.tms_stime))/sysconf(_SC_CLK_TCK);
	printf("User: %f s\n", utime);
	printf("Sys: %f s\n", stime);
	printf("Total: %f s\n", utime + stime);


	return ret;
}

int test_vanilla_kernel(int n_iter, uint64_t limit, const char *file_prefix)
{
	int ret;

	ret = test_set_limit(limit);
	if (ret < 0)
		return ret;

	return test_open_with_time(n_iter, file_prefix);
}

int test_modified_kernel(int n_iter, uint64_t noti_level, uint64_t limit,
			 const char *file_prefix)
{
	int ret;
	int noti_fd;
	int dum;
	pid_t child;
	int child_pipe[2];
	char buf[100];

	ret = test_set_limit(limit);
	if (ret)
		return ret;

	noti_fd = test_netlink();
	if (noti_fd < 0)
		return noti_fd;

	ret = pipe(child_pipe);
	if (ret < 0) {
		log("Unable to create pipe %d", errno);
		goto close_noti;
	}

	log("Forking...");
	ret = fork();
	if (ret == 0) {
		close(child_pipe[1]);
		close(noti_fd);

		read(child_pipe[0], &dum, sizeof(dum));
		close(child_pipe[0]);
		if (dum < 0)
			exit(dum);

		ret = test_vanilla_kernel(n_iter, limit, file_prefix);
		exit(ret);
	} else {
		struct rlimit_event *ev;
		struct rlimit_event_res_changed *res_ev;

		close(child_pipe[0]);
		child = ret;

		log("Configuring FD to get notification after %d descriptors\n",
			noti_level);
		ret = test_register_noti_lvl(noti_fd, child,
					     RLIM_NOFILE, noti_level);
		if (ret < 0)
			goto write_error;

		write(child_pipe[1], &ret, sizeof(ret));
		close(child_pipe[1]);
		waitpid(child, &dum, 0);
		if (WEXITSTATUS(dum) == 0) {
			/* Now let's read the event */
			ret = read(noti_fd, buf, sizeof(buf));
			if (ret < 0) {
				log("Read event failed %d\n", ret);
				goto close_noti;
			}

			ev = (typeof(ev))buf;
			res_ev = (typeof(res_ev))(buf + sizeof(*ev));

			log("Got event: %d, pid: %d, res: %d, new value "
			    "%" PRIu64 "\n",
			    ev->ev_type, res_ev->subj.pid,
			    res_ev->subj.resource, res_ev->new_value);
		}

		close(noti_fd);
	}

	return ret;

write_error:
	write(child_pipe[1], &ret, sizeof(ret));
	close(child_pipe[1]);
close_noti:
	close(noti_fd);
	return ret;
}

static int parse_int(const char *str, int *out)
{
	long int ret;
	char *end;

	ret = strtol(str, &end, 10);
	if (ret == LONG_MAX || ret == LONG_MIN|| *end != '\0')
		return -errno;

	*out = ret;
	return 0;

}

static int parse_uint64(const char *str, uint64_t *out)
{
	unsigned long long int ret;
	char *end;

	ret = strtoull(str, &end, 10);
	if (ret == ULLONG_MAX || *end != '\0')
		return -errno;

	*out = ret;
	return 0;
}

int main(int argc, char **argv)
{
	int ret;

	if (argc < 2) {
		fprintf(stderr, "Usage:\n%s <test_name> [params]\n", argv[0]);
		return -1;
	}

	if (strcmp(argv[0], "rlimit-tests-with-debug") == 0)
		debug_enabled = 1;
	else
		debug_enabled = 0;

	if (strcmp(argv[1], "test_netlink") == 0) {
		ret = test_netlink();
	} else 	if (strcmp(argv[1], "test_register_noti_level") == 0) {
		int noti_fd;
		pid_t pid = getpid();
		int resource = RLIM_NOFILE;
		uint64_t level = 10;

		switch (argc) {
		case 2:
			break;
		case 5:
			ret = parse_uint64(argv[4], &level);
			if (ret)
				goto out;

			/* FALLTHROUGH */
		case 4:
			ret = parse_int(argv[3], &resource);
			if (ret)
				goto out;
			/* FALLTHROUGH */
		case 3:
			ret = parse_int(argv[2], &pid);
			if (ret)
				goto out;

			break;
		default:
			fprintf(stderr, "To much params\n");
			ret = -EINVAL;
			goto out;
		}

		noti_fd = test_netlink();
		if (noti_fd < 0) {
			ret = noti_fd;
			goto out;
		}

		ret = test_register_noti_lvl(noti_fd, pid, resource, level);

	} else	if (strcmp(argv[1], "test_open") == 0) {
		char *file_prefix = "/root/files/file-";
		int n_iter = 40000;

		switch (argc) {
		case 2:
			break;
		case 4:
			ret = parse_int(argv[3], &n_iter);
			if (ret)
				goto out;
			/* FALLTHROUGH */
		case 3:
			file_prefix = argv[2];
			break;
		default:
			fprintf(stderr, "To much params\n");
			ret = -EINVAL;
			goto out;
		}

		ret = test_open(n_iter, file_prefix);

	} else 	if (strcmp(argv[1], "test_open_with_time") == 0) {
		char *file_prefix = "/root/files/file-";
		int n_iter = 40000;

		switch (argc) {
		case 2:
			break;
		case 4:
			ret = parse_int(argv[3], &n_iter);
			if (ret)
				goto out;
			/* FALLTHROUGH */
		case 3:
			file_prefix = argv[2];
			break;
		default:
			fprintf(stderr, "To much params\n");
			ret = -EINVAL;
			goto out;
		}

		ret = test_open_with_time(n_iter, file_prefix);

	} else 	if (strcmp(argv[1], "test_vanilla_kernel") == 0) {
		char *file_prefix = "/root/files/file-";
		uint64_t limit = 50000;
		int n_iter = 40000;

		switch (argc) {
		case 2:
			break;
		case 5:
			file_prefix = argv[4];
			/* FALLTHROUGH */
		case 4:
			ret = parse_uint64(argv[3], &limit);
			if (ret)
				goto out;

			/* FALLTHROUGH */
		case 3:
			ret = parse_int(argv[2], &n_iter);
			if (ret)
				goto out;

			break;
		default:
			fprintf(stderr, "To much params\n");
			ret = -EINVAL;
			goto out;
		}

		ret = test_vanilla_kernel(n_iter, limit, file_prefix);

	} else 	if (strcmp(argv[1], "test_modified_kernel") == 0) {
		char *file_prefix = "/root/files/file-";
		uint64_t limit = 50000;
		uint64_t level = 20;
		int n_iter = 40000;

		switch (argc) {
		case 2:
			break;
		case 6:
			file_prefix = argv[5];
			/* FALLTHROUGH */
		case 5:
			ret = parse_uint64(argv[4], &limit);
			if (ret)
				goto out;

			/* FALLTHROUGH */
		case 4:
			ret = parse_uint64(argv[3], &level);
			if (ret)
				goto out;

			/* FALLTHROUGH */
		case 3:
			ret = parse_int(argv[2], &n_iter);
			if (ret)
				goto out;

			break;
		default:
			fprintf(stderr, "To much params\n");
			ret = -EINVAL;
			goto out;
		}

		ret = test_modified_kernel(n_iter, level, limit, file_prefix);
	} else {
		fprintf(stderr, "Unknown command\n");
		ret = -EINVAL;
	}

out:
	return ret;
}
