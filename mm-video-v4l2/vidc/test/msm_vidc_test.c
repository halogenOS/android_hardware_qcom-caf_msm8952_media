/*
 * Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <utils/Log.h>
#include <string.h>
#include <time.h>
#include <semaphore.h>
#include <poll.h>
#include <queue.h>
#include <ring_queue.h>

#define DEVICE_BASE_MINOR 32
#define MAX_LINE 2048
#define MAX_FILE_PATH_SIZE 128
#define TIMEOUT 20000  /*In milliseconds*/
#define MAX_NUM_BUFS 32

#define EXTRADATA_IDX(__num_planes) (__num_planes - 1)

#define D(fmt, args...) \
	do { \
		if(input_args->verbosity >= 2) \
			printf("D/vidc_test: " fmt, ##args); \
	}while(0)

#define V(fmt, args...) \
	do { \
		if(input_args->verbosity >= 1) \
			printf("V/vidc_test: " fmt, ##args); \
	}while(0)

#define I(fmt, args...) \
			do { \
				printf("I/vidc_test: " fmt, ##args); \
			}while(0)


#define E(fmt, args...) \
	do { \
		printf("E/vidc_test: " fmt, ##args); \
	}while(0)

enum session_type {
	ENCODER_SESSION = 0,
	DECODER_SESSION,
};
enum port_type {
	OUTPUT_PORT = 0,
	CAPTURE_PORT,
	MAX_PORTS
};
enum read_type {
	NOT_ARBITRARY_NUMBER = 0,
	FIX_NUMBER,
	RANDOM_NUMBER,
};
typedef enum status {
	SUCCESS = 0,
	ERROR,
	FAILURE
} status;

enum test_types {
	NOMINAL,
	ADVERSARIAL,
	REPEAT,
	STRESS,
	HELP,
};

typedef enum paramtype {
	STRING,
	INT16,
	INT32,
	FLOAT,
	DOUBLE,
	INT32_ARRAY,
	INT16_ARRAY,
	FLOAT_ARRAY,
	DOUBLE_ARRAY,
	FLAG,
} paramtype;

struct bufinfo {
	int fd;
	__u32 offset;
	__u8* vaddr;
	__u32 size;
	struct ion_handle *handle;
	enum v4l2_buf_type buf_type;
	int index;
};

struct v4l2testappval {
	int fd;
	struct v4l2_requestbuffers bufreq[MAX_PORTS];
	struct v4l2_format fmt[MAX_PORTS];
	struct v4l2_fmtdesc fdesc[MAX_PORTS];
	struct bufinfo binfo[MAX_PORTS][MAX_NUM_BUFS];
	Queue buf_queue[MAX_PORTS];
	ring_buf_header ring_info;
	pthread_mutex_t q_lock[MAX_PORTS];
	pthread_cond_t cond[MAX_PORTS];
	FILE *inputfile,*outputfile,*buf_file;
	pthread_t thread_id[MAX_PORTS];
	pthread_t poll_tid;
	unsigned int ebd_count;
	unsigned int fbd_count;
	int stop_feeding;
	status cur_test_status;
	int events_subscribed;
	int poll_created;
	__u8 *input_buf;
	int verbosity;
};


struct arguments {
	char input[MAX_FILE_PATH_SIZE];
	char output[MAX_FILE_PATH_SIZE];
	char config[MAX_FILE_PATH_SIZE];
	char bufsize_filename[MAX_FILE_PATH_SIZE];
	char device_mode[20];
	int session;
	unsigned long input_height,
		input_width,
		bit_rate,
		frame_rate,
		frame_count,
		stream_format,
		output_order,
		enable_pic_type,
		keep_aspect_ratio,
		post_loop_deblocker,
		divx_format,
		profile,
		level,
		entropy_mode,
		cabac_model,
		idr_period,
		intra_period_p_frames,
		intra_period_b_frames,
		request_i_frame,
		rate_control,
		mb_error_map_reporting,
		loop_filter_mode,
		loop_filter_alpha,
		loop_filter_beta,
		rotation,
		i_frame_qp,
		p_frame_qp,
		b_frame_qp,
		slice_mode,
		slice_mode_mb,
		slice_mode_bytes,
		continue_data_transfer,
		alloc_type,
		enable_frame_assembly,
		n_read_mode,
		read_bytes,
		ring_num_hdrs,
		ring_buf_size,
		random_seed;
	char codec_type[20], read_mode[20];
	char sequence[300][MAX_FILE_PATH_SIZE];
	int verbosity;
	int repeat;
};

typedef struct inputparam {
	const char * param_name;
	paramtype param_type;
	void * param_ptr;
	int array_size;
} param;

static int parse_args(int argc, char **argv);
int parse_cfg(const char *filename);
int parse_param_file(const char * filename, const param * tptab, const int tptab_size);
int parse_sequences(struct arguments *input_args);
static status search_for_preset_param(const param * tptab, const char * param_name, int * pTableIndex, const int table_size);
static size_t find_first_non_whitespace_reverse(const char * str, size_t start_pos);
static int querycap(int fd);
static int enum_formats(int fd, enum v4l2_buf_type buf_type);
static int set_format(int fd, enum v4l2_buf_type buf_type);
static int get_format(int fd, enum v4l2_buf_type buf_type);
static int get_bufreqs(int fd, enum v4l2_buf_type buf_type);
static int qbuf(int fd, enum v4l2_buf_type buf_type);
static int run(int fd);
static int deqbufs(int fd, enum v4l2_buf_type buf_type);
static int streamon(int fd, enum v4l2_buf_type buf_type);
static int streamoff(int fd, enum v4l2_buf_type buf_type);
static int set_control(int fd, struct v4l2_control *control);
static int get_control(int fd, struct v4l2_control *control);
static int subscribe_event(int fd, struct v4l2_event_subscription *sub);
static int unsubscribe_event(int fd, struct v4l2_event_subscription *sub);
static int decoder_cmd(int fd, struct v4l2_decoder_cmd *dec);
static void* poll_func(void *data);
int read_annexb_nalu(FILE * bits, unsigned char * pBuf);
int read_one_frame(FILE * bits, unsigned char * pBuf);
int read_n_bytes(FILE * file, unsigned char * pBuf, int n);
int get_bytes_to_read(void);
static int find_start_code(const unsigned char * pBuf, unsigned int zeros_in_startcode);
static void nominal_test();
static void adversarial_test();
static void repeatability_test();
static void stress_test();


static struct arguments *input_args;
static int ion_fd,sequence_count;
static struct v4l2testappval video_inst;
static int num_of_test_fail;
static int num_of_test_pass;
static const int event_type[] = {
	V4L2_EVENT_MSM_VIDC_FLUSH_DONE,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT,
	V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT,
	V4L2_EVENT_MSM_VIDC_CLOSE_DONE,
	V4L2_EVENT_MSM_VIDC_SYS_ERROR
};
static void (*test_func[]) () = {
	[NOMINAL] = nominal_test,
	[ADVERSARIAL] = adversarial_test,
	[REPEAT] = repeatability_test,
	[STRESS] = stress_test,
};

void help()
{
	printf("\n\n");
	printf("=============================\n");
	printf("msm-vidc-test -c <config file> \n");
	printf("=============================\n\n");
	printf("  eg: msm-vidc-test -c input.cfg \n\n");
	printf("      -c, --config <file>    Configuration file (required)\n");
	printf("      -v, --verbose <#>      0 minimal verbosity,\n");
	printf("                             1 to include details,\n");
	printf("                             2 to debug messages.\n");
	printf("      -n,                    Nominal test (default)\n");
	printf("      -r <#times>,           Repeat test #times\n");
	printf("      -h, --help             Print this menu\n");
	printf("=============================\n\n\n");
}

static int parse_args(int argc, char **argv)
{
	int rc = 0;
	int command;
	struct option longopts[] = {
		{ "nominal",     no_argument,       NULL, 'n'},
		{ "adversarial", no_argument,       NULL, 'a'},
		{ "stress",      no_argument,       NULL, 's'},
		{ "repeat",      required_argument, NULL, 'r'},
		{ "verbose",     required_argument, NULL, 'v'},
		{ "config",      required_argument, NULL, 'c'},
		{ "help",        no_argument,       NULL, 'h'},
		{ NULL,          0,                 NULL,  0},
	};

	while ((command = getopt_long(argc, argv, "nasr:v:c:h", longopts,
				      NULL)) != -1) {
		switch (command) {
		case 'n':
			rc |= 1 << NOMINAL;
			break;
		case 'a':
			rc |= 1 << ADVERSARIAL;
			break;
		case 'r':
			rc |= 1 << REPEAT;
			input_args->repeat = atoi(optarg);
			break;
		case 's':
			rc |= 1 << STRESS;
			break;
		case 'v':
			input_args->verbosity = atoi(optarg);
			break;
		case 'c':
			strlcpy(input_args->config, optarg, MAX_FILE_PATH_SIZE);
			break;
		case 'h':
			help();
			rc |= 1 << HELP;
			break;
		default:
			E("Invalid argument: %c\n", command);
			return -1;
		}
	}
	if (!rc)
		rc = 1 << NOMINAL;
	return rc;
}

int parse_cfg(const char *filename)
{
	int rc = 0;
	param param_table[] = {
		{"device_mode",        STRING,       input_args->device_mode,MAX_FILE_PATH_SIZE},
		{"input_height",       INT32,        &input_args->input_height,MAX_FILE_PATH_SIZE},
		{"input_width",        INT32,        &input_args->input_width,MAX_FILE_PATH_SIZE},
		{"frame_count",        INT32,        &input_args->frame_count,MAX_FILE_PATH_SIZE},
		{"codec_type",         STRING,       input_args->codec_type,MAX_FILE_PATH_SIZE},
		{"input_file",         STRING,       input_args->input,MAX_FILE_PATH_SIZE},
		{"output_file",        STRING,       input_args->output,MAX_FILE_PATH_SIZE},
		{"read_mode",          STRING,       input_args->read_mode,MAX_FILE_PATH_SIZE},
		{"read_bytes",         INT32,        &input_args->read_bytes,MAX_FILE_PATH_SIZE},
		{"random_seed",        INT32,        &input_args->random_seed,MAX_FILE_PATH_SIZE},
		{"fix_buf_size_file",  STRING,       input_args->bufsize_filename,MAX_FILE_PATH_SIZE},
		{"ring_num_headers",   INT32,        &input_args->ring_num_hdrs,MAX_FILE_PATH_SIZE},
		{"ring_buf_size",      INT32,        &input_args->ring_buf_size,MAX_FILE_PATH_SIZE},
		{"eot",                FLAG,          NULL,0}
	};
	rc = parse_param_file(filename, param_table, sizeof(param_table)/sizeof(param_table[0]));
	if (rc) {
		E("Failed to parse param file: %s\n", filename);
		goto err;
	}
	rc = parse_sequences(input_args);
	if (rc) {
		E("Failed to parse sequences\n");
	}
err:
	return rc;
}

static int deqbufs(int fd, enum v4l2_buf_type buf_type)
{
	int rc = 0;
	return rc;
}

int allocate_ion_mem(__u32 size, __u32 align, struct ion_handle **handle)
{
	struct ion_allocation_data alloc_data;
	struct ion_fd_data fd_data;
	int rc;
	alloc_data.len = size;
	alloc_data.align = align;
	alloc_data.heap_mask = ION_HEAP(ION_CP_MM_HEAP_ID) |
				ION_HEAP(ION_IOMMU_HEAP_ID);
	alloc_data.flags = 0;
	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDONLY | O_DSYNC);
	}
	if (ion_fd < 0) {
		E("Failed to open ion device: %d\n", ion_fd);
		return -ENODEV;
	}
	rc = ioctl(ion_fd,ION_IOC_ALLOC,&alloc_data);
	if (rc) {
		E("Failed to allocate ion memory: size %d, align: %d\n",
			alloc_data.len, alloc_data.align);
		return -ENOMEM;
	}
	*handle = fd_data.handle = alloc_data.handle;
	rc = ioctl(ion_fd,ION_IOC_MAP,&fd_data);
	if (rc) {
		E("Failed to MAP ion memory\n");
		/*TODO: Handle error*/
		return -ENOMEM;
	}
	return fd_data.fd;
}

void free_ion_mem(struct bufinfo *binfo)
{
	struct ion_handle_data handle_data;
	handle_data.handle = binfo->handle;
	int rc = ioctl(ion_fd, ION_IOC_FREE, &handle_data);
	if (rc)
		E("Failed to free ion memory: %p, rc: %d\n", binfo->handle, rc);
}

static void free_buffers()
{
	int i,port;
	struct bufinfo *binfo = NULL;
	int check_ring_fd = -1;
	for (port = 0; port < MAX_PORTS; port++) {
		for(i = 0; i < MAX_NUM_BUFS; i++) {
			binfo = &video_inst.binfo[port][i];
			if (binfo->handle && check_ring_fd != binfo->fd) {
				munmap(binfo->vaddr, binfo->size);
				close(binfo->fd);
				free_ion_mem(binfo);
			}
			check_ring_fd = binfo->fd;
		}
	}
}

static int prepare_bufs(int fd, enum v4l2_buf_type buf_type)
{
	int i;
	struct bufinfo *binfo;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];
	int size = 0;
	int numbufs = 0;
	int port = CAPTURE_PORT;

	if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		size = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].sizeimage;
		numbufs = video_inst.bufreq[CAPTURE_PORT].count;
		port = CAPTURE_PORT;
	} else {
		size = video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.plane_fmt[0].sizeimage;
		numbufs = video_inst.bufreq[OUTPUT_PORT].count;
		port = OUTPUT_PORT;
	}

	int align = 4096;
	size = (size + (align-1)) & (~(align-1));
	V("Port: %d: Buffer size required per buffer: %d, num of buf: %d\n",
		port, size, numbufs);
	int rc = 0;
	if (fd < 0) {
		E("Invalid fd: %d\n", fd);
		return -EINVAL;
	}
	for (i=0; i< numbufs; i++) {
		int extra_idx = 0;
		binfo = &video_inst.binfo[port][i];
		binfo->fd = allocate_ion_mem(size, align, &binfo->handle);
		if (binfo->fd < 0) {
			E("Failed to allocate memory\n");
			rc = -ENOMEM;
			break;
		}
		binfo->vaddr = (__u8 *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, binfo->fd, 0);
		if (binfo->vaddr == MAP_FAILED) {
			E("Failed to get buffer virtual address\n");
			rc = -ENOMEM;
			break;
		}
		binfo->buf_type = buf_type;
		binfo->index = i;
		binfo->size = size;
		buf.index = i;
		buf.type = buf_type;
		buf.memory = V4L2_MEMORY_USERPTR;
		plane[0].length = size;
		plane[0].m.userptr = (unsigned long)binfo->vaddr;
		plane[0].reserved[0] = binfo->fd;
		plane[0].reserved[1] = 0;
		plane[0].data_offset = binfo->offset;
		extra_idx = EXTRADATA_IDX(video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes);
		if (port == CAPTURE_PORT &&
			((extra_idx >= 1) && (extra_idx < VIDEO_MAX_PLANES))) {
			plane[extra_idx].length = 0;
			plane[extra_idx].reserved[0] = 0;
			plane[extra_idx].reserved[1] = 0;
			plane[extra_idx].data_offset = 0;
			buf.length = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes;
		} else {
			buf.length = 1;
		}
		buf.m.planes = plane;


		D("Preparing Buffer port:%d : binfo: %p, vaddr: %p, fd: %d\n",
			port, binfo, binfo->vaddr, binfo->fd);
		pthread_mutex_lock(&video_inst.q_lock[port]);
		if(push(&video_inst.buf_queue[port], (void *) binfo) < 0) {
			E("Error in pushing buffers to queue \n");
		}
		pthread_mutex_unlock(&video_inst.q_lock[port]);

		rc = ioctl(fd, VIDIOC_PREPARE_BUF, &buf);
		if (rc) {
			E("Failed to prepare bufs\n");
			break;
		}
	}
	return rc;
}

static int allocate_ring_buffer(int fd, enum v4l2_buf_type buf_type)
{
	int i;
	struct bufinfo *binfo, *binfo_tmp;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];
	int size = 0;
	int ring_size = 0;
	int numbufs = 0;
	int port = OUTPUT_PORT;
	int align = 4096;
	int rc = 0;

	if (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		size = video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.plane_fmt[0].sizeimage;
		numbufs = video_inst.bufreq[OUTPUT_PORT].count;
		port = OUTPUT_PORT;
	} else {
		E("Ring buffer on CAPTURE port not supported\n");
		return -EINVAL;
	}

	if (input_args->ring_buf_size) {
		if ((int)input_args->ring_buf_size < size)
			I("Warning: ring buffer size is less than requested size = %d\n",
				size);
		ring_size = input_args->ring_buf_size;
	}
	else
		ring_size = size * 4;

	ring_size = (ring_size + (align-1)) & (~(align-1));
	if (fd < 0) {
		E("Invalid fd: %d\n", fd);
		return -EINVAL;
	}
	V("Port: %d: Buffer size required per buffer: %d, num of buf: %d, aligned size = %d\n",
		port, size, numbufs, ring_size);
	{
		i = 0;
		int extra_idx = 0;
		binfo = &video_inst.binfo[port][i];
		binfo->fd = allocate_ion_mem(ring_size, align, &binfo->handle);
		if (binfo->fd < 0) {
			E("Failed to allocate memory\n");
			rc = -ENOMEM;
			goto ring_error;
		}
		binfo->vaddr = (__u8 *)mmap(NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, binfo->fd, 0);
		if (binfo->vaddr == MAP_FAILED) {
			E("Failed to get buffer virtual address\n");
			rc = -ENOMEM;
			goto ring_error;
		}
		binfo->buf_type = buf_type;
		binfo->index = i;
		binfo->size = ring_size;
		buf.index = i;
		buf.type = buf_type;
		buf.memory = V4L2_MEMORY_USERPTR;
		plane[0].length = ring_size;
		plane[0].m.userptr = (unsigned long)binfo->vaddr;
		plane[0].reserved[0] = binfo->fd;
		plane[0].reserved[1] = 0;
		plane[0].data_offset = binfo->offset;
		buf.length = 1;
		buf.m.planes = plane;
		V("Preparing Ring Buffer %p\n", binfo->vaddr);
		rc = ioctl(fd, VIDIOC_PREPARE_BUF, &buf);
		if (rc) {
			E("Failed to prepare ring bufer\n");
			goto ring_error;
		}
	}
	for (i = 1; i < numbufs; i++) {
		binfo_tmp = binfo;
		binfo = &video_inst.binfo[port][i];
		memcpy(binfo, binfo_tmp, sizeof(struct bufinfo));
		binfo->index = i;
	}

	pthread_mutex_lock(&video_inst.q_lock[port]);
	for (i = 0; i < numbufs; i++) {
		binfo = &video_inst.binfo[port][i];
		D("push to queue:%d : binfo: %p, vaddr: %p\n",
			port, binfo, binfo->vaddr);
		if (push(&video_inst.buf_queue[port], (void *) binfo) < 0)
			E("Error in pushing buffers to queue \n");
	}
	pthread_mutex_unlock(&video_inst.q_lock[port]);

	// Initialize ring buffer header
	video_inst.ring_info.ring_base_addr = (__u32)binfo->vaddr;
	video_inst.ring_info.ring_size = binfo->size;
	video_inst.ring_info.ring_is_empty = 1;
	video_inst.ring_info.ring_is_full = 0;
	video_inst.ring_info.ring_read_idx = 0;
	video_inst.ring_info.ring_write_idx = 0;

	video_inst.input_buf = (__u8 *)calloc(size, sizeof(__u8));
	if (!video_inst.input_buf) {
		E("input_buf allocation failed\n");
		rc = -1;
		goto ring_error;
	}


ring_error:
	return rc;

}

int configure_session (void)
{
	int is_encode, is_decode;
	I("Input dimensions Height = %ld, Width = %ld, Frame Count = %ld \n",
		input_args->input_height,input_args->input_width,input_args->frame_count);
	I("CODEC type = %s \n",input_args->codec_type);

	is_encode = !strcmp(input_args->device_mode,"ENCODE");
	is_decode = !strcmp(input_args->device_mode,"DECODE");

	if (!is_encode && !is_decode) {
		E("Error \n");
		return -EINVAL;
	}
	if (is_decode) {
		input_args->session = DECODER_SESSION;
		V("decode mode*** \n");
	} else {
		input_args->session = ENCODER_SESSION;
		V("Encode mode*** \n");
	}
	I("Current session is %d\n", input_args->session);

	if (!strcmp(input_args->read_mode, "FIX")) {
		I("Read mode: %s\n", input_args->read_mode);
		input_args->n_read_mode = FIX_NUMBER;
	} else if (!strcmp(input_args->read_mode, "ARBITRARY")) {
		I("Read mode: %s\n", input_args->read_mode);
		input_args->n_read_mode = RANDOM_NUMBER;
	} else {
		I("Read mode: DEFAULT\n");
		input_args->n_read_mode = NOT_ARBITRARY_NUMBER;
	}

	I("Input file: %s\n", input_args->input);
	I("Output file: %s\n", input_args->output);
	if (strncmp(input_args->bufsize_filename, "beefbeef", 8))
		I("fix_buf_size_file: %s\n", input_args->bufsize_filename);
	if (input_args->ring_num_hdrs)
		I("Number of headers to use for OUTPUT port: %ld\n", input_args->ring_num_hdrs);
	if (input_args->ring_buf_size)
		I("Ring buffer size: %ld\n", input_args->ring_buf_size);

	return 0;
}

int commands_controls(void)
{
	int i,pos1,pos2,pos3,pos4,str_len,rc=0;
	char line[MAX_LINE], param_name[128], param_name1[128];
	int fd = -1;
	V("\n \n ****** Commands and Controls ****** \n \n");
	for(i=0; i<sequence_count && !rc; i++) {
		str_len = strlen(input_args->sequence[i]);
		rc = 0;
		pos1 = strcspn(input_args->sequence[i]," ");
		strlcpy(param_name,input_args->sequence[i],pos1+1);
		if(!(strncmp(param_name,"OPEN\r",pos1))) {
			V("OPEN Command\n");
			if (input_args->session == DECODER_SESSION) {
				V("Opening decoder\n");
				fd = open("/dev/video32", O_RDWR);
			} else {
				V("Opening encoder\n");
				fd = open("/dev/video33", O_RDWR);
			}
			if (fd < 0) {
				E("Failed to open video device\n");
				rc = -ENODEV;
				break;
			}
			video_inst.fd = fd;
		} else if(!(strncmp(param_name,"GET_BUFREQ",pos1))) {
			V("GET_BUFREQ Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("GET_BUFREQ on CAPTURE port \n");
				rc = get_bufreqs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to get buffer requirements on capture port\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("GET_BUFREQ on OUTPUT port \n");
				rc = get_bufreqs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to get buffer requirements on output port\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"QUEUE",pos1))) {
			V("QUEUE Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("QUEUE Buffers on CAPTURE port \n");
				rc = qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to queue buffers\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("QUEUE Buffers on OUTPUT port \n");
				rc = qbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to queue buffers\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"RUN\r",pos1))) {
			V("RUN command\n");
			rc = run(fd);
		} else if(!(strncmp(param_name,"DEQUEUE\r",pos1))) {
			rc = deqbufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		} else if(!(strncmp(param_name,"PAUSE\r",pos1))) {
			V("PAUSE Command\n");
		} else if(!(strncmp(param_name,"RESUME\r",pos1))) {
			V("RESUME Command\n");
		} else if(!(strncmp(param_name,"FLUSH\r",pos1))) {
			V("FLUSH Command\n");
		} else if(!(strncmp(param_name,"POLL\r",pos1))) {
			V("POLL Command\n");

		} else if(!(strncmp(param_name,"ENUM_FORMATS",pos1))) {
			V("ENUM_FORMATS Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("ENUM_FORMATS on CAPTURE port \n");
				enum_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("ENUM_FORMATS on OUTPUT port \n");
				enum_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}

		} else if(!(strncmp(param_name,"QUERY_CAP\r",pos1))) {
			V("QUERY_CAP Command**\n");
			rc = querycap(fd);
			if (rc) {
				E("Failed to query capabilities\n");
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"SUBSCRIBE_EVENT\r",pos1))) {
			V("SUBSCRIBE_EVENT Command**\n");
			struct v4l2_event_subscription sub;
			rc=subscribe_event(fd, &sub);
			if (rc) {
				E("Failed to subscribe event \n");
				goto close_fd;
			}
			rc = pthread_create(&video_inst.poll_tid, NULL, poll_func, (void *)fd);
			if (rc) {
				E("Failed to create poll thread: %d\n", rc);
				return rc;
			}
			video_inst.poll_created= 1;
		} else if(!(strncmp(param_name,"UNSUBSCRIBE_EVENT\r",pos1))) {
			V("UNSUBSCRIBE_EVENT Command**\n");
			struct v4l2_event_subscription sub;
			rc=unsubscribe_event(fd, &sub);
			if (rc) {
				E("Failed to unsubscribe event \n");
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"SET_CTRL",pos1))) {
			V("SET_CTRL Command\n");
			struct v4l2_control control;
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			if(!(strncmp(param_name,"FRAME_RATE",pos2))) {
				V("FRAME_RATE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->frame_rate=atoi(param_name);
				V("FRAME_RATE = %ld\n",input_args->frame_rate);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_FRAME_RATE;
				control.value = input_args->frame_rate;
				rc = set_control(fd, &control);
				V("STREAM_FORMAT Set Control Done\n");

			} else if(!(strncmp(param_name,"IDR_PERIOD",pos2))) {
				V("IDR_PERIOD Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->idr_period =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->idr_period);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_IDR_PERIOD;
				control.value = input_args->idr_period;
				rc = set_control(fd, &control);
				V("IDR_PERIOD Set Control Done\n");
			} else if(!(strncmp(param_name,"INTRA_PERIOD_P_FRAMES",pos2))) {
				V("INTRA_PERIOD_P_FRAMES Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->intra_period_p_frames =atoi(param_name);
				V("INTRA_PERIOD_P_FRAMES = %ld\n",input_args->intra_period_p_frames);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_NUM_P_FRAMES;
				control.value = input_args->intra_period_p_frames;
				rc = set_control(fd, &control);
				V("INTRA_PERIOD_P_FRAMES Set Control Done\n");
			} else if(!(strncmp(param_name,"INTRA_PERIOD_B_FRAMES",pos2))) {
				V("INTRA_PERIOD_B_FRAMES Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->intra_period_b_frames =atoi(param_name);
				V("INTRA_PERIOD_B_FRAMES = %ld\n",input_args->intra_period_b_frames);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_NUM_B_FRAMES;
				control.value = input_args->intra_period_b_frames;
				rc = set_control(fd, &control);
				V("INTRA_PERIOD_B_FRAMES Set Control Done\n");
			} else if(!(strncmp(param_name,"REQUEST_I_FRAME",pos2))) {
				V("REQUEST_I_FRAME Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->request_i_frame = atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->request_i_frame);
				//control.id = V4L2_CID_MPEG_VIDC_VIDEO_REQUEST_IFRAME;
				//control.value = input_args->intra_period;
				//rc = set_control(fd, &control);
				V("REQUEST_I_FRAME Set Control Done\n");
				//input_args->request_i_frame = 0;
			} else if(!(strncmp(param_name,"RATE_CONTROL",pos2))) {
				V("RATE_CONTROL Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->rate_control =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->rate_control);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_RATE_CONTROL;
				control.value = input_args->rate_control;
				rc = set_control(fd, &control);
				V("RATE_CONTROL Set Control Done\n");
			} else if(!(strncmp(param_name,"BIT_RATE",pos2))) {
				V("\n BIT_RATE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->bit_rate=atoi(param_name);
				V("\n BIT_RATE = %ld\n",input_args->bit_rate);
				control.id = V4L2_CID_MPEG_VIDEO_BITRATE;
				control.value = input_args->bit_rate;
				rc = set_control(fd, &control);
				V("BIT_RATE Set Control Done\n");
			} else if(!(strncmp(param_name,"ENTROPY_MODE",pos2))) {
				V("ENTROPY_MODE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->entropy_mode =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->entropy_mode);
				control.id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE;
				control.value = input_args->entropy_mode;
				rc = set_control(fd, &control);
				V("ENTROPY_MODE Set Control Done\n");
			} else if(!(strncmp(param_name,"CABAC_MODEL",pos2))) {
				V("CABAC_MODEL Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->cabac_model =atoi(param_name);
				V("CABAC_MODEL = %ld\n",input_args->cabac_model);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_H264_CABAC_MODEL;
				control.value = input_args->cabac_model;
				rc = set_control(fd, &control);
				V("CABAC_MODEL Set Control Done\n");
			} else if(!(strncmp(param_name,"PROFILE",pos2))) {
				V("PROFILE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->profile =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->stream_format);
				control.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
				control.value = input_args->profile;
				rc = set_control(fd, &control);
				V("PROFILE Set Control Done\n");
			} else if(!(strncmp(param_name,"LEVEL",pos2))) {
				V("LEVEL Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->level =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->level);
				control.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
				control.value = input_args->level;
				rc = set_control(fd, &control);
				V("LEVEL Set Control Done\n");
			} else if(!(strncmp(param_name,"ROTATION",pos2))) {
				V("ROTATION Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->rotation =atoi(param_name);
				V("ROTATION = %ld\n",input_args->rotation);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_ROTATION;
				control.value = input_args->rotation;
				rc = set_control(fd, &control);
				V("ROTATION Set Control Done\n");
			} else if(!(strncmp(param_name,"I_FRAME_QP",pos2))) {
				V("I_FRAME_QP Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->i_frame_qp =atoi(param_name);
				V("I_FRAME_QP = %ld\n",input_args->i_frame_qp);
				control.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP;
				control.value = input_args->i_frame_qp;
				rc = set_control(fd, &control);
				V("I_FRAME_QP Set Control Done\n");
			} else if(!(strncmp(param_name,"P_FRAME_QP",pos2))) {
				V("P_FRAME_QP Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->p_frame_qp =atoi(param_name);
				V("P_FRAME_QP = %ld\n",input_args->p_frame_qp);
				control.id = V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP;
				control.value = input_args->p_frame_qp;
				rc = set_control(fd, &control);
				V("P_FRAME_QP Set Control Done\n");
			} else if(!(strncmp(param_name,"B_FRAME_QP",pos2))) {
				V("B_FRAME_QP Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->b_frame_qp =atoi(param_name);
				V("B_FRAME_QP = %ld\n",input_args->b_frame_qp);
				control.id = V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP;
				control.value = input_args->b_frame_qp;
				rc = set_control(fd, &control);
				V("B_FRAME_QP Set Control Done\n");
			} else if(!(strncmp(param_name,"SLICE_MODE",pos2))) {
				V("SLICE_MODE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->slice_mode =atoi(param_name);
				V("SLICE_MODE = %ld\n",input_args->slice_mode);
				control.id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE;
				control.value = input_args->slice_mode;
				rc = set_control(fd, &control);
				V("SLICE_MODE Set Control Done\n");
			} else if(!(strncmp(param_name,"SLICE_MODE_BYTES",pos2))) {
				V("SLICE_MODE_BYTES Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->slice_mode_bytes =atoi(param_name);
				V("SLICE_MODE_BYTES = %ld\n",input_args->slice_mode_bytes);
				control.id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_BYTES;
				control.value = input_args->slice_mode_bytes;
				rc = set_control(fd, &control);
				V("SLICE_MODE_BYTES Set Control Done\n");
			} else if(!(strncmp(param_name,"SLICE_MODE_MB",pos2))) {
				V("SLICE_MODE_MB Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->slice_mode_mb =atoi(param_name);
				V("SLICE_MODE_MB = %ld\n",input_args->slice_mode_mb);
				control.id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB;
				control.value = input_args->slice_mode_mb;
				rc = set_control(fd, &control);
				V("SLICE_MODE_MB Set Control Done\n");
			} else if(!(strncmp(param_name,"LOOP_FILTER_MODE",pos2))) {
				V("LOOP_FILTER_MODE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->loop_filter_mode =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->loop_filter_mode);
				control.id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE;
				control.value = input_args->loop_filter_mode;
				rc = set_control(fd, &control);
				V("LOOP_FILTER_MODE Set Control Done\n");
			} else if(!(strncmp(param_name,"LOOP_FILTER_ALPHA",pos2))) {
				V("LOOP_FILTER_ALPHA Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->loop_filter_alpha =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->loop_filter_alpha);
				control.id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA;
				control.value = input_args->loop_filter_alpha;
				rc = set_control(fd, &control);
				V("LOOP_FILTER_ALPHA Set Control Done\n");
			} else if(!(strncmp(param_name,"LOOP_FILTER_BETA",pos2))) {
				V("LOOP_FILTER_BETA Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->loop_filter_beta =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->loop_filter_beta);
				control.id = V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA;
				control.value = input_args->loop_filter_beta;
				rc = set_control(fd, &control);
				V("LOOP_FILTER_BETA Set Control Done\n");
			} else if(!(strncmp(param_name,"STREAM_FORMAT",pos2))) {
				V("STREAM_FORMAT Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->stream_format =atoi(param_name);
				V("STREAM_FORMAT = %ld\n",input_args->stream_format);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_FORMAT;
				control.value = input_args->stream_format;
				rc = set_control(fd, &control);
				V("STREAM_FORMAT Set Control Done\n");
			} else if(!(strncmp(param_name,"OUTPUT_ORDER",pos2))) {
				V("OUTPUT_ORDER Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->output_order =atoi(param_name);
				V("OUTPUT_ORDER = %ld\n",input_args->output_order);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
				control.value = input_args->output_order;
				rc = set_control(fd, &control);
				V("OUTPUT_ORDER Set Control Done\n");
			} else if(!(strncmp(param_name,"ENABLE_PIC_TYPE",pos2))) {
				V("ENABLE_PIC_TYPE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->enable_pic_type =atoi(param_name);
				V("ENABLE_PIC_TYPE = %ld\n",input_args->enable_pic_type);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_ENABLE_PICTURE_TYPE;
				control.value = input_args->enable_pic_type;
				rc = set_control(fd, &control);
				V("ENABLE_PIC_TYPE Set Control Done\n");
			} else if(!(strncmp(param_name,"KEEP_ASPECT_RATIO",pos2))) {
				V("KEEP_ASPECT_RATIO Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->keep_aspect_ratio =atoi(param_name);
				V("KEEP_ASPECT_RATIO = %ld\n",input_args->keep_aspect_ratio);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_KEEP_ASPECT_RATIO;
				control.value = input_args->keep_aspect_ratio;
				rc = set_control(fd, &control);
				V("KEEP_ASPECT_RATIO Set Control Done\n");
			} else if(!(strncmp(param_name,"POST_LOOP_DEBLOCKER",pos2))) {
				V("POST_LOOP_DEBLOCKER Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->post_loop_deblocker =atoi(param_name);
				V("POST_LOOP_DEBLOCKER = %ld\n",input_args->post_loop_deblocker);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_POST_LOOP_DEBLOCKER_MODE;
				control.value = input_args->post_loop_deblocker;
				rc = set_control(fd, &control);
				V("POST_LOOP_DEBLOCKER Set Control Done\n");
			} else if(!(strncmp(param_name,"DIVX_FORMAT",pos2))) {
				V("DIVX_FORMAT Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->divx_format =atoi(param_name);
				V("DIVX_FORMAT = %ld\n",input_args->divx_format);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_DIVX_FORMAT;
				control.value = input_args->divx_format;
				rc = set_control(fd, &control);
				V("DIVX_FORMAT Set Control Done\n");
			} else if(!(strncmp(param_name,"MB_ERROR_MAP_REPORTING",pos2))) {
				V("MB_ERROR_MAP_REPORTING Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->mb_error_map_reporting =atoi(param_name);
				V("MB_ERROR_MAP_REPORTING = %ld\n",input_args->mb_error_map_reporting);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_MB_ERROR_MAP_REPORTING;
				control.value = input_args->mb_error_map_reporting;
				rc = set_control(fd, &control);
				V("MB_ERROR_MAP_REPORTING Set Control Done\n");
			} else if(!(strncmp(param_name,"CONTINUE_DATA_TRANSFER",pos2))) {
				V("CONTINUE_DATA_TRANSFER Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->continue_data_transfer =atoi(param_name);
				V("CONTINUE_DATA_TRANSFER = %ld\n",input_args->continue_data_transfer);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
				control.value = input_args->continue_data_transfer;
				rc = set_control(fd, &control);
				V("CONTINUE_DATA_TRANSFER (smooth streaming) Set Control Done\n");
			} else if(!(strncmp(param_name,"ALLOC_TYPE",pos2))) {
				V("ALLOC_TYPE Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->alloc_type =atoi(param_name);
				if(input_args->alloc_type == V4L2_MPEG_VIDC_VIDEO_RING)
					V("Ring buffer allocation \n");
				else if(input_args->alloc_type == V4L2_MPEG_VIDC_VIDEO_STATIC)
					V("Static buffer allocation \n");
				else
					E("Incorrect ALLOC_TYPE\n");
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_ALLOC_MODE_INPUT;
				control.value = input_args->alloc_type;
				rc = set_control(fd, &control);
				V("ALLOC_TYPE Set Control Done\n");
			} else if(!(strncmp(param_name,"FRAME_ASSEMBLY",pos2))) {
				V("FRAME_ASSEMBLY Control\n");
				pos3 = strcspn(input_args->sequence[i]+pos1+1+pos2+1," ");
				strlcpy(param_name,input_args->sequence[i]+pos1+1+pos2+1,pos3+1);
				input_args->enable_frame_assembly =atoi(param_name);
				V("FRAME_ASSEMBLY = %ld\n",input_args->enable_frame_assembly);
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_FRAME_ASSEMBLY;
				control.value = input_args->enable_frame_assembly;
				rc = set_control(fd, &control);
				V("FRAME_ASSEMBLY Set Control Done\n");
			} else {
				E("ERROR .... Wrong Control \n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"GET_CTRL\r",pos1))) {
			V("GET_CTRL Command\n");
			struct v4l2_control control;
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			if(!(strncmp(param_name,"STREAM_FORMAT\r",pos2))) {
				V("STREAM_FORMAT Control\n");
				control.id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_FORMAT;
				rc = get_control(fd, &control);
				V("STREAM_FORMAT Get Control Done\n");
			} else {
				E("ERROR .... Wrong Control \n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"CLOSE\r",pos1))) {
			V("CLOSE Command\n");
			/*if (fd >= 0)
				close(fd);
			if (ion_fd >= 0)
				close(ion_fd);*/
		} else if(!(strncmp(param_name,"STREAM_ON",pos1))) {
			V("STREAM_ON Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);

			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("CAPTURE PORT Stream ON \n");
				rc = streamon(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to call streamon on V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("OUTPUT PORT Stream ON \n");
				rc = streamon(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to call streamon on V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}

		} else if(!(strncmp(param_name,"STREAM_OFF",pos1))) {
			V("STREAM_OFF Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}

			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);

			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("CAPTURE PORT Stream OFF \n");
				rc = streamoff(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to call streamoff on capture port\n");
					goto close_fd;
				}

			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("OUTPUT PORT Stream OFF \n");
				rc = streamoff(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to call streamoff on capture port\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"SET_FMT",pos1))) {
			V("SET_FMT Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}

			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);

			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("SET_FMT on CAPTURE port \n");
				rc = set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to set format on capture port\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("SET_FMT on OUTPUT port \n");
				rc = set_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to set format on output port\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}

		} else if(!(strncmp(param_name,"GET_FMT",pos1))) {
			V("GET_FMT Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;

				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);

			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("GET_FMT on CAPTURE port \n");
				rc = get_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to get format on capture port\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("GET_FMT on OUTPUT port \n");
				rc = get_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to get format on capture port\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}

		} else if(!(strncmp(param_name,"PREPARE_BUFS",pos1))) {
			V("PREPARE_BUFS Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			if (pos2 == 0) {
				if (pos1+1 == str_len) {
					E("No Port Specified \n");
					rc = -EINVAL;
					goto close_fd;
				}
				do {
					pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
					pos1++;
				} while(pos1+1 < str_len && pos2 == 0);
				pos1--;
			}

			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);

			if(!(strncmp(param_name,"CAPTURE\r",pos2))) {
				V("PREPARE_BUFS on CAPTURE port \n");
				rc = prepare_bufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
				if (rc) {
					E("Failed to prepare buffers on capture port\n");
					goto close_fd;
				}
			} else if (!(strncmp(param_name,"OUTPUT\r",pos2))) {
				V("PREPARE_BUFS on OUTPUT port \n");
				rc = prepare_bufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
				if (rc) {
					E("Failed to prepare buffers on output port\n");
					goto close_fd;
				}
			} else {
				E("Wrong port sepcified\n");
				rc = -EINVAL;
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"ALLOC_RING_BUF", pos1))) {
			V("ALLOC_RING_BUF Command\n");
			rc = allocate_ring_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
			if (rc) {
				E("Failed to allcate ring buffer on output port\n");
				goto close_fd;
			}
		} else if(!(strncmp(param_name,"SLEEP",pos1))) {
			int sleep_time = 0;
			V("SLEEP Command\n");
			pos2 = strcspn(input_args->sequence[i]+pos1+1," ");
			strlcpy(param_name,input_args->sequence[i]+pos1+1,pos2+1);
			sleep_time = atoi(param_name);
			V("Sleeping for %d\n", sleep_time);
			sleep(sleep_time);
			V("Waking up!\n");

		} else {
			E("WRONG Command\n");
			rc = -EINVAL;
			goto close_fd;
		}
	}
	return rc;
close_fd:
	//	close(fd);
	return rc;
}

int parse_sequences(struct arguments *input_args)
{
	FILE * fp;
	char line[MAX_LINE], param_name[128], arr_seps[]=" ,\t\n", * token, * str_wbuf;
	size_t pos1, pos2, pos3, len;
	int iTableIndex, arr_read,is_sequence;
	float flt;
	size_t str_wlen;
	int temp32;
	short * p16;
	iTableIndex = 0;
	sequence_count = 0;
	if(!(fp = fopen(input_args->config,"rt"))) {
		return -EIO;
	}
	while(fgets(line,MAX_LINE-1,fp)) {
		len = strlen(line);
		pos1 = strcspn(line, "#");
		pos2 = strcspn(line,":");
		if(pos2 == len)
			continue;
		line[pos2] = '\t';
		sscanf(line,"%s",param_name);
		is_sequence=strcmp(param_name,"SEQUENCE");
		if(is_sequence != 0)
			continue;
		for( ; pos2<len; pos2++) {
			if(line[pos2]!=' '&& line[pos2]!='\t' && line[pos2]!='\r')
				break;
		}
		pos3 = pos2 + find_first_non_whitespace_reverse(line+pos2,pos1 - pos2);
		str_wbuf = input_args->sequence[iTableIndex++];
		str_wlen = pos3 - pos2 + 1;
		strlcpy(str_wbuf,line+pos2,str_wlen+1);
		sequence_count++;
	}
	fclose(fp);
	return 0;
}


int parse_param_file(const char * filename, const param * tptab, const int tptab_size)
{
	FILE * fp;
	char line[MAX_LINE], param_name[128],arr_seps[]=" ,\t\n", * token, * str_wbuf;
	size_t pos1, pos2, pos3, len;
	int iTableIndex, arr_read;
	float flt;
	size_t str_wlen;
	int temp32;
	short * p16;
	if(!(fp = fopen(filename,"rt"))) {
		E("fail to open configuration file: %s\n", filename);
		return -EIO;
	}
	while(fgets(line,MAX_LINE-1,fp)) {
		len = strlen(line);
		pos1 = strcspn(line, "#");
		pos2 = strcspn(line,":");
		if(pos2 == len)
			continue;
		line[pos2] = '\t';
		sscanf(line,"%s",param_name);
		if(search_for_preset_param(
				tptab,param_name,&iTableIndex,tptab_size) != SUCCESS) {
			if(iTableIndex >= tptab_size || iTableIndex < 0) {
				E("Malformed param_table[]\n");
				return -EOVERFLOW;
			}
			continue;
		}
		switch((tptab+iTableIndex)->param_type) {
		case INT32:
			sscanf(line+pos2+1,"%d",(int*)(tptab+iTableIndex)->param_ptr);
			break;
		case INT16:
			sscanf(line+pos2+1,"%d",&temp32);
			p16 = ((short*)(tptab+iTableIndex)->param_ptr);
			*p16 = (short)temp32;
			break;
		case FLOAT:
			sscanf(line+pos2+1,"%f",&flt);
			*(float*)(tptab+iTableIndex)->param_ptr = flt;
			break;
		case DOUBLE:
			sscanf(line+pos2+1,"%f",&flt);
			*(double*)(tptab+iTableIndex)->param_ptr = flt;
			break;
		case INT32_ARRAY:
		case INT16_ARRAY:
		case FLOAT_ARRAY:
		case DOUBLE_ARRAY:
			arr_read = 0;
			token = strtok(line+pos2,arr_seps);
			while(token != NULL && arr_read < (tptab+iTableIndex)->array_size) {
				if((tptab+iTableIndex)->param_type == INT32_ARRAY)
					((int*)(tptab+iTableIndex)->param_ptr)[arr_read++] = atoi(token);
				else if((tptab+iTableIndex)->param_type == INT16_ARRAY)
					((short*)(tptab+iTableIndex)->param_ptr)[arr_read++] = atoi(token);
				else if((tptab+iTableIndex)->param_type == FLOAT_ARRAY)
					((float*)(tptab+iTableIndex)->param_ptr)[arr_read++] = (float)atof(token);
				else
					((double*)(tptab+iTableIndex)->param_ptr)[arr_read++] = atof(token);
				token = strtok( NULL, arr_seps );
			}
			break;
		case STRING:
			for( ; pos2<len; pos2++) {
				if(line[pos2]!=' '&& line[pos2]!='\t' && line[pos2]!='\r')
					break;
			}
			pos3 = pos2 + find_first_non_whitespace_reverse(line+pos2,pos1 - pos2);

			str_wbuf = (char*)(tptab+iTableIndex)->param_ptr;
			str_wlen = pos3 - pos2 + 1;
			if((int)str_wlen > (tptab+iTableIndex)->array_size - 1) {
				str_wlen = (tptab+iTableIndex)->array_size - 1;
			}
			strlcpy(str_wbuf,line+pos2,str_wlen+1);
			break;
		case FLAG:
			break;
		}
	}
	fclose(fp);
	return 0;
}

static status search_for_preset_param(const param * tptab, const char * param_name, int * pTableIndex, const int table_size)
{
	const param * p = tptab;
	* pTableIndex = -1;
	while(strcmp(p->param_name,"eot")) {
		(*pTableIndex) ++;
		if(!strcmp(p->param_name,param_name)) {
			return SUCCESS;
		}
		p++;
		if(*pTableIndex >= table_size)
			return ERROR;
	}
	return ERROR;
}

static size_t find_first_non_whitespace_reverse(const char * str, size_t start_pos)
{
	size_t len = strlen(str);
	int pos;

	pos = (start_pos > len - 1) ? len - 1 : start_pos;
	if(pos == 0)
		return pos;

	for(; pos-- >=0 ;) {
		if(str[pos] != ' ' && str[pos] != '\t' && str[pos]!='\r')
			break;
	}
	return pos;
}

static int set_control(int fd, struct v4l2_control *control)
{
	int rc;
	D("Calling IOCTL set control for id=%d, val=%d\n", control->id, control->value);
	rc = ioctl(fd, VIDIOC_S_CTRL, control);
	if (rc) {
		E("Failed to set control id=%d, val=%d\n", control->id, control->value);;
		return -1;
	}
	D("Success IOCTL set control for id=%d, value=%d\n", control->id, control->value);
	return 0;
}

static int get_control(int fd, struct v4l2_control *control)
{
	int rc;
	D("Calling IOCTL get control for id=%d\n", control->id);
	rc = ioctl(fd, VIDIOC_G_CTRL, control);
	if (rc) {
		E("Failed to get control\n");
		return -1;
	}
	D("Success IOCTL get control for id=%d, value=%d\n", control->id, control->value);
	return 0;
}

static int subscribe_event(int fd, struct v4l2_event_subscription *sub)
{
	int i, rc;
	int size_event = sizeof(event_type)/sizeof(int);
	for (i = 0; i < size_event; i++) {
		memset(sub, 0, sizeof(*sub));
		sub->type = event_type[i];
		D("Calling IOCTL to Subscribe Event id = %d \n", sub->type);
		rc = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, sub);
		if (rc) {
			E("Failed to get control\n");
			return -1;
		}
		V("Success IOCTL to Subscribe Event id = %d \n", sub->type);
		video_inst.events_subscribed += 1;
	}
	return 0;
}

static int unsubscribe_event(int fd, struct v4l2_event_subscription *sub)
{
	int i, rc;
	int size_event = sizeof(event_type)/sizeof(int);
	for (i = 0; i < size_event; i++) {
		sub->type = event_type[i];
		D("Calling IOCTL to UnSubscribe Event id = %d \n", sub->type);
		rc = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, sub);
		if (rc) {
			E("Failed to UnSubscribe event\n");
			return -1;
		}
		V("Success IOCTL to UnSubscribe Event id = %d \n", sub->type);
		video_inst.events_subscribed -= 1;
	}
	return 0;
}

static int decoder_cmd(int fd, struct v4l2_decoder_cmd *dec)
{
	int rc;
	V("Calling IOCTL to Decoder cmd id = %d \n", dec->cmd);
	rc = ioctl(fd, VIDIOC_DECODER_CMD, dec);
	if (rc) {
		E("Failed to get control\n");
		return -1;
	}
	return 0;
}

static int enum_formats(int fd, enum v4l2_buf_type buf_type)
{
	struct v4l2_fmtdesc fdesc;
	fdesc.type = buf_type;
	int i, rc;
	for(i = 0;; i++) {
		fdesc.index = i;
		rc = ioctl(fd, VIDIOC_ENUM_FMT, &fdesc);
		if (rc)
			break;
		V("Enum fmt: description: %s, fmt: %x, flags = %x\n", fdesc.description,
			fdesc.pixelformat, fdesc.flags);
	}
	return 0;
}

static int get_format(int fd, enum v4l2_buf_type buf_type)
{
	struct v4l2_format fmt;
	int port;
	int rc;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = buf_type;
	rc = ioctl(fd, VIDIOC_G_FMT, &fmt);
	if (rc) {
		E("Failed to get format\n");
		return -1;
	}
	port = buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAPTURE_PORT : OUTPUT_PORT;
	V("VIDIOC_G_FMT port = %d: height = %d, width = %d, format = %x, size = %d\n",
		port,
		fmt.fmt.pix_mp.height,
		fmt.fmt.pix_mp.width,
		fmt.fmt.pix_mp.pixelformat,
		fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
	V("stride = %d, scanlines = %d, num_planes = %d\n",
		fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
		fmt.fmt.pix_mp.plane_fmt[0].reserved[0],
		fmt.fmt.pix_mp.num_planes);

	switch(port) {
	case CAPTURE_PORT :
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.height=fmt.fmt.pix_mp.height;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.width=fmt.fmt.pix_mp.width;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.pixelformat=fmt.fmt.pix_mp.pixelformat;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].sizeimage=fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes=fmt.fmt.pix_mp.num_planes;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].bytesperline=
			fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].reserved[0]=
			fmt.fmt.pix_mp.plane_fmt[0].reserved[0];
		//TODO: Add extra data info
		break;
	case OUTPUT_PORT :
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.height=fmt.fmt.pix_mp.height;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.width=fmt.fmt.pix_mp.width;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.pixelformat=fmt.fmt.pix_mp.pixelformat;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.plane_fmt[0].sizeimage=fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.num_planes=fmt.fmt.pix_mp.num_planes;
		break;
	default :
		break;
	}
	return 0;
}

static int get_v4l2_format(char *fmt_str)
{
	int fmt = V4L2_PIX_FMT_H264;
	if (!fmt_str) {
		E("Invalid input\n");
		return -EINVAL;
	}
	if (!strcmp(fmt_str, "H.264")) {
		fmt = V4L2_PIX_FMT_H264;
	} else if (!strcmp(fmt_str, "MPEG4")) {
		fmt = V4L2_PIX_FMT_MPEG4;
		V("\n MPEG4 Selected \n ");
	} else if (!strcmp(fmt_str, "VP8")) {
		fmt = V4L2_PIX_FMT_VP8;
	} else {
		E("Unrecognized format string. Defaulting to H264\n");
	}
	return fmt;
}

static int set_format(int fd, enum v4l2_buf_type buf_type)
{
	struct v4l2_format fmt;
	int port;
	int format = 0;
	int rc = 0;
	if (input_args->session == DECODER_SESSION) {
		if (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
			format = get_v4l2_format(input_args->codec_type);
		} else if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			format = V4L2_PIX_FMT_NV12;
		} else {
			E("Invalid port\n");
			return -EINVAL;
		}
	} else if (input_args->session == ENCODER_SESSION) {
		V("Inside ENCODER SESSION set_format\n");
		if (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
			format = V4L2_PIX_FMT_NV12;
		} else if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			format = get_v4l2_format(input_args->codec_type);
		} else {
			E("Invalid port\n");
			return -EINVAL;
		}
	} else {
		E("Invalid session");
		return -EINVAL;
	}
	port = buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAPTURE_PORT : OUTPUT_PORT;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = buf_type;
	fmt.fmt.pix_mp.height = input_args->input_height;
	fmt.fmt.pix_mp.width = input_args->input_width;
	fmt.fmt.pix_mp.pixelformat = format;
	V("VIDIOC_S_FMT port = %d: h: %d, w: %d\n",
		port, fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.width);
	rc = ioctl(fd, VIDIOC_S_FMT, &fmt);
	if (rc) {
		E("Failed to set format\n");
		return -1;
	}
	D("height = %d, width = %d, format = 0x%x, size = %d\n", fmt.fmt.pix_mp.height,
		fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.pixelformat,
		fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
	switch(port) {
	case CAPTURE_PORT :
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.height=fmt.fmt.pix_mp.height;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.width=fmt.fmt.pix_mp.width;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.pixelformat=fmt.fmt.pix_mp.pixelformat;
		video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].sizeimage=fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
		break;
	case OUTPUT_PORT :
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.height=fmt.fmt.pix_mp.height;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.width=fmt.fmt.pix_mp.width;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.pixelformat=fmt.fmt.pix_mp.pixelformat;
		video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.plane_fmt[0].sizeimage=fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
		break;
	default :
		break;
	}
	return rc;
}

static int querycap(int fd)
{
	struct v4l2_capability cap;
	int rc;
	rc = ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (rc) {
		E("Failed to query capabilities\n");
	} else {
		V("Capabilities: driver_name = %s, card = %s, bus_info = %s,"
			" version = %d, capabilities = %x\n", cap.driver, cap.card,
			cap.bus_info, cap.version, cap.capabilities);
	}
	return rc;
}

static int get_bufreqs(int fd, enum v4l2_buf_type buf_type)
{
	struct v4l2_requestbuffers bufreq;
	int port;
	int rc;

	port = buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAPTURE_PORT : OUTPUT_PORT;
	if (port == OUTPUT_PORT && input_args->ring_num_hdrs)
		bufreq.count = input_args->ring_num_hdrs;
	else
		bufreq.count = 2;
	bufreq.type = buf_type;
	bufreq.memory = V4L2_MEMORY_USERPTR;
	rc = ioctl(fd, VIDIOC_REQBUFS, &bufreq);
	if (rc) {
		E("Failed to query bufreqs\n");
		return -1;
	}

	V("VIDIOC_REQBUFS port = %d: count = %d\n", port, bufreq.count);
	switch(port) {
	case CAPTURE_PORT :
		video_inst.bufreq[CAPTURE_PORT].count = bufreq.count;
		video_inst.bufreq[CAPTURE_PORT].type = bufreq.type;
		break;
	case OUTPUT_PORT :
		video_inst.bufreq[OUTPUT_PORT].count = bufreq.count;
		video_inst.bufreq[OUTPUT_PORT].type = bufreq.type;
		break;
	default :
		break;
	}
	return 0;
}

static int streamon(int fd, enum v4l2_buf_type buf_type)
{
	int rc;
	int port;
	port = buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAPTURE_PORT : OUTPUT_PORT;
	D("VIDIOC_STREAMON port = %d\n", port);
	rc = ioctl(fd, VIDIOC_STREAMON, &buf_type);
	if (rc) {
		E("Failed to call streamon\n");
	}
	return rc;
}

static int streamoff(int fd, enum v4l2_buf_type buf_type)
{
	int rc;
	struct timespec timeout;
	enum v4l2_buf_type btype;
	int port;
	if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		port = CAPTURE_PORT;
	} else if (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		port = OUTPUT_PORT;
	} else {
		E("Capability not supported\n");
		return -EINVAL;
	}
	if (port == OUTPUT_PORT) {
		btype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		pthread_join(video_inst.thread_id[OUTPUT_PORT], NULL);
	} else {
		btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (!video_inst.stop_feeding) {
			while (1) {
				pthread_mutex_lock(&video_inst.q_lock[port]);
				rc = pthread_cond_wait(&video_inst.cond[port],
						&video_inst.q_lock[port]);
				if (video_inst.stop_feeding) {
					pthread_mutex_unlock(&video_inst.q_lock[port]);
					break;
				}
				pthread_mutex_unlock(&video_inst.q_lock[port]);
			}
		}
	}
	D("VIDIOC_STREAMOFF port = %d\n", port);
	rc = ioctl(fd, VIDIOC_STREAMOFF, &btype);
	if (rc) {
		E("Failed to call streamoff on port %d\n", port);
	}
	return rc;
}

int read_yuv_nv12(FILE *filp, unsigned char *pbuf)
{
	int rc = 0;
	int size = 0;
	int stride = 0;
	if (!filp || !pbuf) {
		E("Invalid input\n");
		return -EINVAL;
	}
	stride = (input_args->input_width + 31) & (~31);
	size = input_args->input_height * stride * 3/2;
	rc = fread (pbuf, 1, size, filp);
	return rc;
}
static int q_single_buf(struct bufinfo *binfo)
{
	int rc = 0;
	struct v4l2_buffer buf;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];
	int eos = 0;
	int port = MAX_PORTS;
	int extra_idx = 0;
	int read_size = 0;
	int bytes_to_read = 0;
	if(!binfo) {
		E("Invalid binfo: %p\n", binfo);
		return -EINVAL;
	}
	port = binfo->buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
			 OUTPUT_PORT : CAPTURE_PORT;
	D("%s: fill port: %d data\n", __FUNCTION__, port);

	memset(&buf, 0, sizeof(buf));
	if (port == OUTPUT_PORT) {
		if (input_args->session == DECODER_SESSION) {
			if (input_args->alloc_type == V4L2_MPEG_VIDC_VIDEO_RING) {
				if (input_args->n_read_mode) {
					bytes_to_read = get_bytes_to_read();
					V("Reading arbitrary bytes = %d\n", bytes_to_read);
					rc = read_n_bytes(video_inst.inputfile,
							video_inst.input_buf,
							 bytes_to_read);
				} else {
					V("copy read frame to input buffer\n");
					rc = read_one_frame(video_inst.inputfile, video_inst.input_buf);
				}
			} else
				rc = read_one_frame(video_inst.inputfile, binfo->vaddr);
		} else if (input_args->session == ENCODER_SESSION) {
			rc = read_yuv_nv12(video_inst.inputfile, binfo->vaddr);
		} else {
			E("Invalid session\n");
			return -EINVAL;
		}
		if (rc < 0) {
			E("Error reading file\n");
			return -EINVAL;
		} else if (rc == 0) {
			V("EOS:END OF STREAM\n");
			eos = 1;
			buf.flags = V4L2_QCOM_BUF_FLAG_EOS;
		}
		read_size = rc;
		if (input_args->alloc_type == V4L2_MPEG_VIDC_VIDEO_RING) {
			binfo->offset = ring_buf_write(&video_inst.ring_info,
							video_inst.input_buf,
							(__u32*)&read_size);
			D("ETB: offset = %d\n", binfo->offset);
			if (read_size < rc) {
				E("Error writing to ring buffer: %d \n", read_size);
				return -EINVAL;
			}
		}
		D("ETB: bytesused: %d\n", read_size);
	}

	buf.index = binfo->index;
	buf.type = binfo->buf_type;
	buf.memory = V4L2_MEMORY_USERPTR;
	plane[0].bytesused = read_size;
	plane[0].length = binfo->size;
	plane[0].m.userptr = (unsigned long)binfo->vaddr;
	plane[0].reserved[0] = binfo->fd;
	plane[0].reserved[1] = 0;
	plane[0].data_offset = binfo->offset;
	extra_idx = EXTRADATA_IDX(video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes);
	if (port == CAPTURE_PORT) {
		if ((extra_idx >= 1) && (extra_idx < VIDEO_MAX_PLANES)) {
			plane[extra_idx].length = 0;
			plane[extra_idx].reserved[0] = 0;
			plane[extra_idx].reserved[1] = 0;
			plane[extra_idx].data_offset = 0;
			buf.length = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes;
		} else
			buf.length = 1;
	} else {
		buf.length = 1;
	}
	buf.m.planes = plane;
	D("Queueing:%d, port:(%d) fd = %d, userptr = %p,"
		" offset = %d, flags=0x%x, bytesused= %d, length= %d\n",
		video_inst.fd, port,
		plane[0].reserved[0], (void *)plane[0].m.userptr,
		plane[0].data_offset, buf.flags,
		plane[0].bytesused, plane[0].length);
	rc = ioctl(video_inst.fd, VIDIOC_QBUF, &buf);
	if (rc) {
		rc = -errno;
		E("Failed to qbuf to driver rc = %d\n", rc);
	}
	if(video_inst.ebd_count == input_args->request_i_frame) {
		struct v4l2_control control;
		control.id = V4L2_CID_MPEG_VIDC_VIDEO_REQUEST_IFRAME;
		control.value = 1;
		rc = set_control(video_inst.fd, &control);
		input_args->request_i_frame = -1;
	}

	if (eos) {
		rc = -1; //Don't modify unless you know what are you doing
	}
	D("EXIT: %s: rc = %d\n", __FUNCTION__, rc);
	return rc;
}

static int qbuf(int fd, enum v4l2_buf_type buf_type)
{
	int rc = 0;
	int port;
	if (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		port = CAPTURE_PORT;
	} else if (buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		port = OUTPUT_PORT;
	} else {
		E("Capability not supported\n");
		return -EINVAL;
	}
	struct bufinfo *binfo = pop(&video_inst.buf_queue[port]);
	if (binfo) {
		rc = q_single_buf(binfo);
		if (rc == -1) {
			D("EOS received exit thread, port: %d\n", port);
		}
		else if (rc) {
			E("Failed to q buf exit thread, port: %d\n", port);
		}
	} else
		V("Buffer queue empty, port: %d\n", port);
	return rc;
}

static void* queue_func(void *data)
{

	struct bufinfo *binfo;
	int port = (int)data;
	int rc = 0,ret = 0;
	struct timespec timeout;
	struct timeval now;
	D("queue_func thread port: %d\n", port);
	while(1) {
		pthread_mutex_lock(&video_inst.q_lock[port]);

		if (video_inst.stop_feeding) {
			D("Aborting the session\n");
			pthread_mutex_unlock(&video_inst.q_lock[port]);
			break;
		}

		binfo = pop(&video_inst.buf_queue[port]);
		D("pop'ed binfo (%d)\n", port);
		pthread_mutex_unlock(&video_inst.q_lock[port]);

		if(binfo == NULL) {
			D("No free Buffer to queue. Waiting(%d)... \n", port);
			pthread_mutex_lock(&video_inst.q_lock[port]);

			gettimeofday(&now,NULL);
			timeout.tv_sec = now.tv_sec+TIMEOUT/1000;
			timeout.tv_nsec = now.tv_usec;
			rc = pthread_cond_timedwait(&video_inst.cond[port],
							&video_inst.q_lock[port], &timeout);
			if (rc == ETIMEDOUT) {
				E("pthread condition wait timedout, port: %d\n", port);
				pthread_mutex_unlock(&video_inst.q_lock[port]);
				break;
			}
			binfo = pop(&video_inst.buf_queue[port]);
			D("pop'ed binfo after wait (%d)\n", port);
			pthread_mutex_unlock(&video_inst.q_lock[port]);
		}
		if (!rc && binfo) {
			rc = q_single_buf(binfo);
			if (rc == -1) {
				D("EOS received exit thread, port: %d\n", port);
			}
			else if (rc) {
				E("Failed to q buf exit thread, port: %d\n", port);
				D("Requesting stop_feeding in all port threads\n");
				video_inst.stop_feeding = 1;
				break;
			}
			D("q_single_buf(%d) succeed\n", port);
		} else {
			E("Error in getting free buffer: %d, %p\n", rc, binfo);
			break;
		}
	}
	D("Exiting queue_func: %d\n", port);
	return 0;
}

static void* poll_func(void *data)
{
	struct bufinfo *binfo;
	int num_bytes_written=0;
	struct pollfd pfd;
	int fd = (int) data;
	int rc = 0;
	struct v4l2_event dqevent;
	struct v4l2_buffer v4l2_buf;
	int stride;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];
	int filled_len = 0;

	stride = (input_args->input_width + 31) & (~31);
	pfd.fd = fd;
	pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLRDBAND | POLLPRI;
	while(1) {
		rc = poll(&pfd, 1, TIMEOUT);
		if (!rc) {
			E("Poll timedout\n");
			break;
		} else if (rc < 0) {
			E("Error while polling: %d\n", rc);
			break;
		}
		D("events = 0x%x\n", pfd.revents);
		if ((pfd.revents & POLLIN) || (pfd.revents & POLLRDNORM)) {
			v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			v4l2_buf.memory = V4L2_MEMORY_USERPTR;
			v4l2_buf.length = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.num_planes;
			v4l2_buf.m.planes = plane;
			while(!ioctl(pfd.fd, VIDIOC_DQBUF, &v4l2_buf)) {
				binfo = &video_inst.binfo[CAPTURE_PORT][v4l2_buf.index];
				pthread_mutex_lock(&video_inst.q_lock[CAPTURE_PORT]);
				++video_inst.fbd_count;
				if (v4l2_buf.flags & V4L2_QCOM_BUF_FLAG_EOS ||
					video_inst.fbd_count >= input_args->frame_count) {
					if (v4l2_buf.flags & V4L2_QCOM_BUF_FLAG_EOS)
						V("Received eos on capture port = 0x%x\n", v4l2_buf.flags);
					else
						V("frame_count reached = %d\n", video_inst.fbd_count);
					video_inst.stop_feeding = 1;
					video_inst.cur_test_status = SUCCESS;
				}
				filled_len = plane[0].bytesused;
				D("FBD COUNT: %d, filled length = %d\n", video_inst.fbd_count, filled_len);
				if (input_args->session == DECODER_SESSION && filled_len) {
					int stride = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].bytesperline;
					int scanlines = video_inst.fmt[CAPTURE_PORT].fmt.pix_mp.plane_fmt[0].reserved[0];
					const char *temp = (const char *)plane[0].m.userptr;
					unsigned i;
					int bytes_written = 0;
					for (i = 0; i < input_args->input_height; i++) {
						bytes_written = fwrite(temp, input_args->input_width, 1, video_inst.outputfile);
						temp += stride;
					}
					temp = (const char *)binfo->vaddr + stride * scanlines;
					for(i = 0; i < input_args->input_height/2; i++) {
						bytes_written += fwrite(temp, input_args->input_width, 1, video_inst.outputfile);
						temp += stride;
					}
					D("Written %d bytes successfully\n", bytes_written);
				} else if (input_args->session == ENCODER_SESSION) {
					rc = fwrite((const char *)binfo->vaddr,
							filled_len,1,video_inst.outputfile);
					if (rc > 0) {
						D("Written %d bytes successfully\n", rc);
					} else {
						if (!filled_len)
							E("Failed to write output\n");
					}
				}
				rc = push(&video_inst.buf_queue[CAPTURE_PORT], (void *) binfo);
				if (rc) {
					E("Failed(rc = %d) to push buffer into: %d queue\n", rc, CAPTURE_PORT);
					pthread_mutex_unlock(&video_inst.q_lock[CAPTURE_PORT]);
					break;
				}
				pthread_cond_broadcast(&video_inst.cond[CAPTURE_PORT]);
				pthread_mutex_unlock(&video_inst.q_lock[CAPTURE_PORT]);
			}
		} else if ((pfd.revents & POLLOUT) || (pfd.revents & POLLWRNORM)) {
			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(&plane, 0, sizeof(plane));
			v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			v4l2_buf.memory = V4L2_MEMORY_USERPTR;
			v4l2_buf.m.planes = plane;
			v4l2_buf.length = video_inst.fmt[OUTPUT_PORT].fmt.pix_mp.num_planes;
			D("dqbuf:port(0)\n");
			while (!ioctl(pfd.fd, VIDIOC_DQBUF, &v4l2_buf)) {
				__u32 nOffset = 0;
				binfo = &video_inst.binfo[OUTPUT_PORT][v4l2_buf.index];
				pthread_mutex_lock(&video_inst.q_lock[OUTPUT_PORT]);
				++video_inst.ebd_count;
				D("EBD COUNT: %d\n", video_inst.ebd_count);
				if (input_args->alloc_type == V4L2_MPEG_VIDC_VIDEO_RING) {
					D("Getting offset... port (0)\n");
					nOffset = v4l2_buf.m.planes[0].data_offset;
					D("port(0): new ring buf offset: %d\n", nOffset);
					D("port(0): v4l2 index: %d, binfo index: %d\n",
						v4l2_buf.index, binfo->index);
					rc = ring_buf_read(&video_inst.ring_info, NULL, nOffset);
					if (rc)
						E("Error reading ring buffer\n");
					else
						D("Sucess updating ring offset\n");
				}
				D("push buf in port (0)\n");
				rc = push(&video_inst.buf_queue[OUTPUT_PORT], (void *) binfo);
				if (rc) {
					E("Failed(rc = %d) to push buffer into: %d queue\n", rc, OUTPUT_PORT);
					pthread_mutex_unlock(&video_inst.q_lock[OUTPUT_PORT]);
					break;
				}
				pthread_cond_broadcast(&video_inst.cond[OUTPUT_PORT]);
				pthread_mutex_unlock(&video_inst.q_lock[OUTPUT_PORT]);
			}
		} else if (pfd.revents & POLLRDBAND) {
			D("TODO: Some event has happened\n");
		} else if (pfd.revents & POLLPRI) {
			rc = ioctl(pfd.fd, VIDIOC_DQEVENT, &dqevent);
			if(dqevent.type == V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT ) {
				V("\n Port Reconfig recieved insufficient\n");
			} else if (dqevent.type == V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT) {
				V("\n Setting changed sufficient\n");
			} else if (dqevent.type == V4L2_EVENT_MSM_VIDC_FLUSH_DONE) {
				V("\n Flush Done Recieved \n");
			} else if (dqevent.type == V4L2_EVENT_MSM_VIDC_CLOSE_DONE) {
				V("\n Close Done Recieved \n");
				break;
			} else if(dqevent.type == V4L2_EVENT_MSM_VIDC_SYS_ERROR) {
				E("\n SYS Error Recieved \n");
				break;
			} else {
				E("\n Some event has occurred!! Type: 0x%x\n", dqevent.type);
				continue;
			}
		} else if (pfd.revents & POLLERR) {
			if (!video_inst.events_subscribed) {
				break;
			}
		} else {
			E("TODO: An error occured\n ");
			break;
		}
	}
	D("EXIT poll()\n");
	return NULL;
}

static int run(int fd)
{
	int rc = 0;
	//In success poll_func() will change the status
	video_inst.cur_test_status = FAILURE;

	rc = pthread_create(&video_inst.thread_id[OUTPUT_PORT], NULL, queue_func, (void *)OUTPUT_PORT);
	if (rc) {
		E("Failed to create output queue thread: %d\n", rc);
		return rc;
	}
	rc = pthread_create(&video_inst.thread_id[CAPTURE_PORT], NULL, queue_func, (void *)CAPTURE_PORT);
	if (rc) {
		E("Failed to create capture queue thread: %d\n", rc);
		return rc;
	}
	if (!video_inst.poll_created) {
		rc = pthread_create(&video_inst.poll_tid, NULL, poll_func, (void *)fd);
		if (rc) {
			E("Failed to create poll thread: %d\n", rc);
			return rc;
		}
	}
	return rc;
}

static void nominal_test()
{
	int rc = 0;
	int i;
	struct v4l2_decoder_cmd dec;
	ion_fd = -1;
	video_inst.cur_test_status = SUCCESS;

	rc = parse_cfg(input_args->config);
	if (rc) {
		E("Failed to parse configuration file\n");
		help();
		return;
	}
	video_inst.inputfile = fopen(input_args->input,"rb");
	if (!video_inst.inputfile) {
		E("Failed to open input file %s\n", input_args->input);
		goto err;
	}
	video_inst.outputfile = fopen(input_args->output,"wb");
	if (!video_inst.outputfile) {
		E("Failed to open output file %s\n", input_args->output);
		goto fail_op_file;
	}

	if ((strncmp(input_args->bufsize_filename, "beefbeef", 8))) {
		video_inst.buf_file = fopen(input_args->bufsize_filename, "rb");
		if (!video_inst.buf_file) {
			E("Failed to buffer sizes file %s\n", input_args->bufsize_filename);
			goto fail_buf_file;
		}
	}

	V("Setting seed: srand(%lu) \n", input_args->random_seed);
	srand(input_args->random_seed);
	for (i = 0; i < MAX_PORTS; i++) {
		pthread_mutex_init(&video_inst.q_lock[i], 0);
		pthread_cond_init(&video_inst.cond[i], 0);
	}
	rc = configure_session();
	if (rc) {
		E("Failed to configure session\n");
		goto fail_config_session;
	}

	rc = commands_controls();
	if (rc) {
		E("Failed to set controls\n");
		goto fail_config_session;
	}

	V("Waiting for threads to join\n");
	for (i = 0; i < MAX_PORTS; i++) {
		pthread_join(video_inst.thread_id[i], NULL);
	}

	if (input_args->session == DECODER_SESSION) {
		V("Flush OUTPUT port \n");
		dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
		dec.flags = V4L2_DEC_QCOM_CMD_FLUSH_OUTPUT;
		rc = decoder_cmd(video_inst.fd, &dec);
		if (rc)
			E("Failed to flush OUTPUT port\n");

		V("Flush CAPTURE port \n");
		dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
		dec.flags = V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE;
		rc = decoder_cmd(video_inst.fd, &dec);
		if (rc)
			E("Failed to flush CAPTURE port\n");


		V("Stopping...\n");
		dec.cmd = V4L2_DEC_CMD_STOP;
		rc = decoder_cmd(video_inst.fd, &dec);
	}

	V("CAPTURE PORT Stream OFF \n");
	rc = streamoff(video_inst.fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (rc)
		E("Failed to call streamoff on capture port\n");



	V("OUTPUT PORT Stream OFF \n");
	rc = streamoff(video_inst.fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (rc)
		E("Failed to call streamoff on capture port\n");

	V("Waiting for poll to join\n");
	pthread_join(video_inst.poll_tid, NULL);
fail_config_session:
	for (i = 0; i < MAX_PORTS; i++) {
		pthread_mutex_destroy(&video_inst.q_lock[i]);
	}
	if (video_inst.buf_file)
		fclose(video_inst.buf_file);
fail_buf_file:
	fclose(video_inst.outputfile);
fail_op_file:
	fclose(video_inst.inputfile);

err:
	if (video_inst.cur_test_status == SUCCESS && !rc) {
		num_of_test_pass++;
		I("Test passed\n");
	} else {
		num_of_test_fail++;
		I("Test fail\n");
	}
	close(video_inst.fd);
	free_buffers();
	close(ion_fd);
	//De-initialize global variables for repetability test
	D("Clear Global variables\n");
	video_inst.ebd_count = 0;
	video_inst.fbd_count = 0;
	video_inst.stop_feeding = 0;
	video_inst.events_subscribed = 0;
	video_inst.poll_created = 0;
	video_inst.cur_test_status = FAILURE;
	for (i = 0; i < MAX_PORTS; i++)
		free_queue(&video_inst.buf_queue[i]);

}

static void adversarial_test()
{
	I("adversial_test not implemented yet\n");
}

static void repeatability_test()
{
	int test_number;
	for (test_number = 1; test_number <= input_args->repeat; test_number++) {
		I("Test #%d \n", test_number);
		nominal_test();
	}
}

static void stress_test()
{
	I("stress_test not implemented yet\n");
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int i;
	int test_mask;


	input_args = (struct arguments *)malloc(sizeof(struct arguments));
	if (input_args == NULL)
		return -1;
	/* Setting defaults */
	memset(input_args, 0, sizeof(*input_args));
	input_args->request_i_frame = -1;
	input_args->verbosity = 1;
	strlcpy(input_args->bufsize_filename, "beefbeef", MAX_FILE_PATH_SIZE);
	test_mask = parse_args(argc, argv);
	if (test_mask < 0) {
		E("Failed to parse args\n");
		help();
		goto err;
	}

	if (test_mask & (1U << HELP))
		goto err;

	for (i = 0; i < (int)(sizeof(test_func)/sizeof(*(test_func))); i++) {
		/* Look for the test that was selected */
		if (!(test_mask & (1U << i)))
			continue;
		/* This test was selected, so run it */
		I("Test type selected: \n");
		switch (i) {
		case NOMINAL:
			I("Nominal (default) test\n");
			break;
		case ADVERSARIAL:
			I("Adversarial\n");
			break;
		case REPEAT:
			I("Repeatability\n");
			break;
		case STRESS:
			I("Stress\n");
			break;
		case HELP:
			I("HELP\n");
			break;
		default:
			E("Not listed test\n");
		break;
		}
		test_func[i] ();
	}

	I("\n\n\t\tTest results:\n");
	if (!num_of_test_fail && !num_of_test_pass)
		I("\tNo test executed\n");
	else {
		I("\tTest pass: %d\n", num_of_test_pass);
		I("\tTest fail: %d\n\n\n", num_of_test_fail);
	}
	if(num_of_test_fail)
		rc = -1;

err:
	free(input_args);
	return rc;
}

int read_mpeg4_chunk(FILE * bits, unsigned char * pBuf)
{
	unsigned char temp_buf[2048];
	const int mp4StartCode = 0x000001B6;
	static int first_time=0;
	int findStartCode = 0;
	int startCodeFound = 0;
	int read_size = 0;
	int curr_ptr = 0;
	int word = 0,i;
	int length = 0;
	if(first_time == 1) {
		pBuf[0]=0;
		pBuf[1]=0;
		pBuf[2]=1;
		pBuf[3]=0xb6;
		pBuf+=4;
	}
	while (startCodeFound == 0 && (!feof(bits))) {
		read_size = fread(temp_buf, 1,256, bits);
		if (read_size == 0) {
			V("\n EOF reached \n");
			break;
		}
		curr_ptr = 0;
		do {
			word = (word << 8) | temp_buf[curr_ptr++];
			findStartCode = (word == mp4StartCode);
			if(findStartCode) {
				if (startCodeFound) {
					length -= 3;
				} else {
					startCodeFound = 1;
					length++;
				}
				break;
			} else
				length++;
		} while(curr_ptr < read_size);
		memcpy(pBuf, temp_buf, curr_ptr);
		pBuf+=curr_ptr;
	}
	fseek (bits, (int)(curr_ptr - read_size), SEEK_CUR);
	if(first_time == 0) {
		length -= 4;
		first_time = 1;
	}
	return length;
}

int read_vp8_chunk(FILE * bits, unsigned char * pBuf)
{
	int read_length;
	unsigned long long time_stamp;
	static int ivf_header_read = 0;
	if(ivf_header_read == 0) {
		read_length = fread (pBuf, 1, 32, bits);
		V("\n Read IVF HEADER \n ");
		ivf_header_read = 1;
		if(pBuf[0] == 'D' && pBuf[1] == 'K' && pBuf[2] == 'I' && pBuf[3] == 'F') {
			V(" \n IVF header found \n ");
		} else {
			V(" \n No IVF header found \n ");
			lseek((int)bits, -32, SEEK_CUR);
		}
	}
	if( 1 != fread (&read_length, 4, 1, bits)) {
		V("\n input EOS Reached \n");
		return 0;
	}
	D("\n Read Length =%d \n",read_length);
	fread (&time_stamp, 8, 1, bits);
	D("\n Time Stamp =%lld \n",time_stamp);
	fread (pBuf, 1, read_length, bits);
	return read_length;
}

int read_annexb_nalu(FILE * bits, unsigned char * pBuf)
{
	int info2, info3, pos = 0;
	int StartCodeFound, rewind;
	static int first_time = 0;
	info2 = 0;
	info3 = 0;
	if (3 != fread (pBuf, 1, 3, bits)) {
		return 0;
	}
	info2 = find_start_code(pBuf, 2);
	if (info2 != 1) {
		if(1 != fread(pBuf+3, 1, 1, bits)) {
			return 0;
		}
		info3 = find_start_code(pBuf, 3);
	}
	if (info2 != 1 && info3 != 1) {
		E ("get_annexb_nalu: no Start Code at the begin of the NALU, return -1\n");
		return -1;
	}
	if( info2 == 1) {
		pos = 3;
	} else if(info3 ==1 ) {
		pos = 4;
	} else {
		E( " Panic: Error \n");
		return -1;
	}
	StartCodeFound = 0;
	info2 = 0;
	info3 = 0;
	while (!StartCodeFound) {
		if (feof (bits)) {
			return pos-1;
		}

		pBuf[pos++] = fgetc (bits);
		info3 = find_start_code(pBuf+pos-4, 3);
		if(info3 != 1)
			info2 = find_start_code(pBuf+pos-3, 2);
		StartCodeFound = (info2 == 1 || info3 == 1);
		if (StartCodeFound && first_time < 2) {
			++first_time;
			StartCodeFound = 0;
		}
	}
	// Here, we have found another start code (and read length of startcode bytes more than we should
	// have.  Hence, go back in the file
	rewind = 0;
	if(info3 == 1)
		rewind = -4;
	else if (info2 == 1)
		rewind = -3;
	else
		E(" Panic: Error in next start code search \n");

	if (0 != fseek (bits, rewind, SEEK_CUR)) {
		fprintf(stderr,"GetAnnexbNALU: Cannot fseek %d in the bit stream file", rewind);
	}
	return (pos+rewind);
}

static int find_start_code(const unsigned char * pBuf, unsigned int zeros_in_startcode)
{
	unsigned int info = 1, i;

	for (i = 0; i < zeros_in_startcode; i++) {
		if(pBuf[i] != 0)
			info = 0;
	}

	if(pBuf[i] != 1)
		info = 0;
	return info;
}

int read_one_frame(FILE * bits, unsigned char * pBuf)
{
	int read_length;
	if (!strcmp(input_args->codec_type, "H.264")) {
		read_length = read_annexb_nalu(bits,pBuf);
	} else if (!strcmp(input_args->codec_type, "MPEG4")) {
		read_length = read_mpeg4_chunk(bits,pBuf);
	} else if (!strcmp(input_args->codec_type, "VP8")) {
		read_length = read_vp8_chunk(bits,pBuf);
	} else {
		E("\n Unrecognised CODECS \n");
		read_length = -1;
	}
	return read_length;
}

int read_n_bytes(FILE * file, unsigned char * pBuf, int n)
{
	int read_bytes = 0;

	read_bytes = fread (pBuf, 1, n, file);
	if (read_bytes != n) {
		if (feof(file))
			V("info: EOF: as been reach\n");
		else
			E("Error: Reading file\n");
	}
	return read_bytes;
}

int get_bytes_to_read(void)
{
	int bytes = 0;
	char line[MAX_LINE];
	int max_read_size = (input_args->input_width * input_args->input_height * 3/2)/2;
	int max_numbufs = video_inst.bufreq[OUTPUT_PORT].count;
	static unsigned int index = 0;
	static int is_read_done = 0;

	if (input_args->n_read_mode == RANDOM_NUMBER) {
		/* Trying to queue at least one frame in the available buffers minus 1*/
		V("bytes_to_read shall be > %d\n", max_read_size/(max_numbufs-1));
		do {
			bytes = rand() % max_read_size;
			V("try rand(): bytes = %d\n", bytes);
		} while (bytes < (max_read_size/(max_numbufs-1)));
	} else { /* FIX_NUMBER */
		/* Read initial values from file then continue using: input_args->read_bytes */
		if (strncmp(input_args->bufsize_filename, "beefbeef", 8) && !is_read_done) {
			if(fgets(line, MAX_LINE-1, video_inst.buf_file)) {
				bytes = atoi(line);
				index++;
			} else {
				is_read_done = 1;
				V("No more data in file: %s, values read: %d\n",
					input_args->bufsize_filename, index);
			}
		}
		if (!strncmp(input_args->bufsize_filename, "beefbeef", 8) ||
				(strncmp(input_args->bufsize_filename, "beefbeef", 8) && is_read_done)) {
			bytes = input_args->read_bytes;
			if(!bytes || bytes > max_read_size) {
				V("Warning: Limit bytes to read!! bytes = %d\n", max_read_size);
				bytes = max_read_size;
			}
		}
	}
	return bytes;
}
