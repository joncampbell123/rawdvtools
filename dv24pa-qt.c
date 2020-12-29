#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <linux/soundcard.h>	// OSS audio ioctls
#include <linux/fb.h>		// Linux framebuffer ioctls
#include <libdv/dv.h>		// libdv video decoder

#ifndef O_BINARY
#define O_BINARY 0
#endif

unsigned long long lseek64(int fd,unsigned long long x,int whence);

typedef struct {
	unsigned char		raw[80];
	unsigned char		SCT,Arb,Dseq,FSC,DBN;	// decoded from DIF header
} DIFchunk;

static DIFchunk ReadDIFtmp;
static int tracking = 0;	/* 0....9 */
static int video_frames = 0;
static int emit_frames = 0;

DIFchunk *PickDIF(unsigned char *raw)
{
	DIFchunk *d = &ReadDIFtmp;

	memcpy(d->raw,raw,80);

	/* pick apart DIF header */
	d->SCT =   d->raw[0] >> 5;
	d->Arb =   d->raw[0]       & 0xF;
	d->Dseq =  d->raw[1] >> 4;
	d->FSC =  (d->raw[1] >> 3) & 1;
	d->DBN =   d->raw[2];
	return d;
}

static int die = 0;
void sigma(int x) {
	if (x == SIGINT || x == SIGQUIT || x == SIGTERM)
		die = 1;
}

static char *truefalse[2] = {"false","true"};
static unsigned char cur_frame[12000*12];
static unsigned char frame[12000*12],frame_blk[150*12];

static inline void wr_be64(unsigned char *dst,uint64_t val) {
	dst[7] = val; val >>= 8;
	dst[6] = val; val >>= 8;
	dst[5] = val; val >>= 8;
	dst[4] = val; val >>= 8;

	dst[3] = val; val >>= 8;
	dst[2] = val; val >>= 8;
	dst[1] = val; val >>= 8;
	dst[0] = val;
}

static inline void wr_be32(unsigned char *dst,uint32_t val) {
	dst[3] = val; val >>= 8;
	dst[2] = val; val >>= 8;
	dst[1] = val; val >>= 8;
	dst[0] = val;
}

static inline void wr_be16(unsigned char *dst,uint16_t val) {
	dst[1] = val; val >>= 8;
	dst[0] = val;
}

void qt_write_atom_ext(int fd,char *atom,unsigned long long size) {
	unsigned char buffer[16];
	wr_be32(buffer,1);	/* indicates extended size follows */
	memcpy(buffer+4,atom,4);
	wr_be64(buffer+8,size);
	write(fd,buffer,16);
}

void qt_write_atom(int fd,char *atom,unsigned long size) {
	unsigned char buffer[8];
	wr_be32(buffer,size);	/* indicates extended size follows */
	memcpy(buffer+4,atom,4);
	write(fd,buffer,8);
}

static unsigned long long current(int fd) {
	return lseek64(fd,0,SEEK_CUR);
}

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=0,prevseq=0,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;
	unsigned long current_alen=0;
	int fb_offset = 0;
	int fbx=0,fby=0;
	int fbfd = -1;
	int fbhalf = 0;

	if (argc < 3) {
		fprintf(stderr,"dv24pa-qt [options] <in .dv> <out .dv>\n");
		fprintf(stderr,"Convert raw DV containing 24p advanced to QuickTime with 24p pulldown\n");
		fprintf(stderr,"  -o offset (in frames)\n");
		return 1;
	}

	signal(SIGINT,sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

	char *dvfile = NULL;
	char *outfile = NULL;
	unsigned long long offset = -1;
	unsigned long long poffset = -1;
	int offset_adv = 1;
	{
		int i,nosw=0;

		for (i=1;i < argc;) {
			char *a = argv[i++];

			if (*a == '-') {
				char *sw = a+1;

				if (0) {
				}
				else {
					fprintf(stderr,"Unknown switch %s\n",sw);
					return 1;
				}
			}
			else {
				int i = nosw++;

				switch (i) {
					case 0:
						dvfile = a;
						break;
					case 1:
						outfile = a;
						break;
					default:
						fprintf(stderr,"Duhhhh what do I do with extra param %s?\n",a);
						return 1;
				}
			}
		}
	}

	memset(frame,0xFF,12000*10);
	memset(frame_blk,0,150*10);

	/* open output */
	int mov_fd = open64(outfile,O_RDWR|O_CREAT|O_TRUNC);
	if (mov_fd < 0) {
		fprintf(stderr,"Cannot open output file\n");
		return 1;
	}

	/* cool. now scan the stream for other header blocks and pick out time codes */
	{
		fd = open64(dvfile,O_RDONLY | O_BINARY);
		if (fd < 0) return 1;

		unsigned long long file_size = lseek64(fd,0,SEEK_END);
		offset = 0;

		time_t began = time(NULL);
		while (!die) {
			// ugh okay so this is a cheap way to do it, but... it works
			// note that tracking is used here for those rare
			// weird cases where dvgrab gets things misaligned
			if (lseek64(fd,offset+(tracking*12000),SEEK_SET) != (offset+(tracking*12000)))
				break;
			if (read(fd,cur_frame,12000*10) < 12000)
				break;

			// use the packet numbering to auto-track
			{
				int N[10];
				int i,consistent=0,offset;

				{
					DIFchunk *c = PickDIF(cur_frame);
					offset = c->Dseq;
				}
				for (i=1;i < 10;i++) {
					DIFchunk *c = PickDIF(cur_frame + (i * 12000));
					if (((c->Dseq + (10 - offset)) % 10) == i) {
						consistent++;
					}
				}

				if (offset != 0) {
					if (consistent >= 7) {
						printf("\nRaw DV tracking: Apparently whole frames are off by %d rows\n",offset);
						tracking = (tracking + (10 - offset)) % 10;
						continue;
					}
				}
			}

			int tH = -1,tM = -1,tS = -1,tF = -1;
			int pat24p = -1;

			fprintf(stdout,"\x0D" "29.97fps/23.976fps: %d/%d      ",video_frames,emit_frames);
			fflush(stdout);

			memset(frame_blk,0,sizeof(frame_blk));
			memset(frame,0,12000 * 12);
			for (asx=0;asx < (150*10);asx++) {
				DIFchunk *c = PickDIF(cur_frame + (asx * 80));
				if (!c) continue;
				int bo = c->Dseq * 150;

				if (c->SCT == 0)
					bo += 0;
				else if (c->SCT == 1)
					bo += c->DBN + 1;
				else if (c->SCT == 2)
					bo += c->DBN + 3;
				else if (c->SCT == 3)
					bo += (c->DBN * 16) + 6;
				else if (c->SCT == 4)
					bo += (c->DBN + (c->DBN / 15)) + 7;

				if (bo < (150 * 12) && !frame_blk[bo]) {
					memcpy(frame + (bo * 80),c->raw,80);
					frame_blk[bo] = 1;
				}

				// pick out the time code
				if (c->SCT == 1) {
					unsigned char *SSYB,Syb,*PC;
					int SN;

					for (SN=0;SN < 8;SN++) {
						SSYB = c->raw + 3 + (SN * 8);
						if (SSYB[2] != 0xFF) continue;
						PC = SSYB + 3;

						Syb = SSYB[1] & 0xF;
						if ((Syb == 3 || Syb == 9) && PC[0] == 0x13) {
							tH = (PC[4] & 0xF) + (((PC[4] >> 4) & 3) * 10);
							tM = (PC[3] & 0xF) + (((PC[3] >> 4) & 7) * 10);
							tS = (PC[2] & 0xF) + (((PC[2] >> 4) & 7) * 10);
							tF = (PC[1] & 0xF) + (((PC[1] >> 4) & 3) * 10);
						}
					}
				}
				// VAUX
				else if (c->SCT == 2) {
					unsigned char *PC;
					int SN;

					for (SN=0;SN < 15;SN++) {
						PC = c->raw + 3 + (SN * 5);
						
						if (PC[0] == 0x64 && pat24p < 0) {
							unsigned char telecine = PC[3];
							if (telecine < 5) pat24p = telecine;
//							fprintf(stderr,"0x63: %02X %02X %02X %02X %02X\n",
//								PC[0],PC[1],PC[2],PC[3],PC[4]);
						}
					}
				}
			}

			/* estimate the corresponding 24p frame count */
			int video24_frames = (video_frames * 4) / 5;
			int do_emit = 0;

			if (emit_frames < (video24_frames-1) && pat24p >= 0) {
				fprintf(stderr,"Emitting 24p duplicate frame anyway, because video is behind\n");
				pat24p = -1;
			}

			if (emit_frames > (video24_frames+2)) {
				fprintf(stderr,"Not emitting frame, 24p or no, because video is ahead\n");
			}
			else if (pat24p == 2) {
				/* skip no. 2, because that is the duplicate frame */
			}
			else {
				/* emit */
				do_emit = 1;
			}

			if (do_emit) {
				write(mov_fd,frame,12000*10);
				emit_frames++;
			}

			/* carry on */
			video_frames++;

			/* advance */
			offset += 12000*10;
		}
	}

	close(mov_fd);
	close(fd);
	return 0;
}

