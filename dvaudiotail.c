#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>

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

static char *truefalse[2] = {"false","true"};
static unsigned char cur_frame[12000*12];
static unsigned char frame[12000*12],frame_blk[150*12];

void WriteWAV(int fd,unsigned long datalen)
{
	unsigned long r1 = datalen - 8;
	unsigned long r2 = datalen - 44;
	unsigned short w;
	unsigned int d;

	lseek64(fd,0,SEEK_SET);
	write(fd,"RIFF",4);
	write(fd,&r1,4);
	write(fd,"WAVEfmt ",8);
	d = 0x0010; write(fd,&d,4);
	w = 0x0001; write(fd,&w,2);
	w =      2; write(fd,&w,2);
	d =  48000; write(fd,&d,4);
	d *=     4; write(fd,&d,4);
	w =      4; write(fd,&w,2);
	w =     16; write(fd,&w,2);
	write(fd,"data",4);
	write(fd,&r2,4);
}

static signed short audio[1620*2];
static int audio_len=0;
static int video_frames=0;
static int audio_level = 0;

static int upsample12(int y)
{
	int shift = (y >> 8) & 0xF;

	if (shift >= 2 && shift < 8) {
		shift--;
		return (y - (shift<<8)) << shift;
	}
	else if (shift >= 8 && shift <= 0xd) {
		shift = 0xe - shift;
		return ((y + ((shift<<8)+1)) << shift) - 1;
	}

	return y;
}

static unsigned long audio_shouldbe()
{
	// calculate according to standard 1600, 1602, 1602, 1602, 1602 pattern
	unsigned long x = (video_frames * 1602) - ((video_frames / 5) * 2);
	return x;
}

signed short bsw16(signed short x)
{
	unsigned short t = (unsigned)x;
	return (signed)((t >> 8) | (t << 8));
}

int dec_lastsample[2] = {0,0};
int quants[8] = {16,12,-1,-1,-1,-1,-1,-1};
int srates[4] = {48000,44100,32000,-1};
void DecodeAudio()
{
	unsigned char *blk;
	int i,j,bn,N,ch,quant=-1,srate=-1;

	audio_len = 1600;
	for (i=0;i < 1620*2;i++)
		audio[i] = 0x8000;

	// how many samples?
	for (i=0;i < 10;i++) {
		for (j=0;j < 9;j++) {
			bn = (i * 150) + (j << 4) + 6;
			blk = frame + (bn * 80);

			if ((blk[0] >> 5) == 3 && blk[3] == 0x50) {
				unsigned as = blk[4] & 0x3F;
				if (as == 0x14)		audio_len = 1600;
				else if (as == 0x15)	audio_len = 1601;
				else if (as == 0x16)	audio_len = 1602;
				else if (as == 0x0E)	audio_len = 1067;
				else if (as == 0x0F)	audio_len = 1068;
				else			printf("Code 0x%02X?\n",as);

				quant = quants[blk[7] & 7];
				srate = srates[(blk[7] >> 3) & 3];
			}
		}
	}

	if (quant < 0 || srate < 0)
		return;

	// de-interleave and assemble
	if (quant == 16) {
		for (N=0;N < 1620;N++) {
			int ds1,ds2,da,b,bn1,bn2;
			signed short *ptr;

			ds1 = ((N / 3) + ((N % 3) * 2)) % 5;
			ds2 = ds1 + 5;
			da  = (3 * (N % 3)) + ((N % 45) / 15);
			b   = 2 * (N / 45);
			bn1 = (ds1 * 150) + (da << 4) + 6;
			bn2 = (ds2 * 150) + (da << 4) + 6;

			if (frame_blk[bn1]) {
				ptr = (signed short*)(frame + (bn1 * 80) + 8 + b);
				audio[N<<1] = bsw16(*ptr);
			}

			if (frame_blk[bn2]) {
				ptr = (signed short*)(frame + (bn2 * 80) + 8 + b);
				audio[(N<<1)+1] = bsw16(*ptr);
			}
		}
	}
	else if (quant == 12) {
		static int dv_audio_unshuffle_60[5][9] = {
			{ 0, 15, 30, 10, 25, 40,  5, 20, 35 },
			{ 3, 18, 33, 13, 28, 43,  8, 23, 38 },
			{ 6, 21, 36,  1, 16, 31, 11, 26, 41 },
			{ 9, 24, 39,  4, 19, 34, 14, 29, 44 },
			{12, 27, 42,  7, 22, 37,  2, 17, 32 }};
		unsigned char *blk;
		int stride = 45;
		int bp,i,i_base;
		unsigned char my,mz,l;
		int ds,dif,ch;

		for (ch=0;ch < 1;ch++) {
			for (ds=0;ds < 5;ds++) {
				for (dif=0;dif < 9;dif++) {
					int dsb = ds + (ch * 5);
					int bn = (dsb * 150) + (dif << 4) + 6;
					signed short *aptr = audio + ch;

					if (frame_blk[bn]) {
						blk = frame + (bn * 80);
						i_base = dv_audio_unshuffle_60[ds][dif];
						for (bp=8;bp < 80;bp += 3) {
							i = i_base + (bp - 8)/3 * stride;
							my = blk[bp];
							mz = blk[bp+1];
							l  = blk[bp+2];

							int y = (my << 4) | (l >> 4);
							int z = (mz << 4) | (l & 0xF);
							if (y > 2048) y -= 4096;
							if (z > 2048) z -= 4096;

							if (y == 2048)	y = 0x8000;
							else		y = upsample12(y);

							if (z == 2048)	z = 0x8000;
							else		z = upsample12(z);

							aptr[i<<1] = y;
							aptr[(i<<1)+1] = z;
						}
					}
				}
			}
		}
	}

	// resample to 48000Hz if needed
	if (srate != 48000 && audio_len != 0 && srate > 0) {
		signed short or[audio_len*2];
		int i=0,o=0,sc=0;

		memcpy(or,audio,audio_len*2*2);
		while (o < (1620*2) && i < (audio_len*2)) {
			audio[o++] = or[i];
			audio[o++] = or[i+1];
			sc += srate;
			if (sc >= 48000) {
				sc -= 48000;
				i += 2;
			}
		}

		audio_len = o>>1;
	}
}

void RemoveDamaged()
{
	unsigned char *blk;
	int i,j,bn,N,ch;

	// remove damaged samples
	for (ch=0;ch < 2;ch++) {
		for (i=0;i < audio_len;i++) {
			if (audio[ch+(i<<1)] == ((signed short)0x8000))
				audio[ch+(i<<1)] = dec_lastsample[ch];
			else
				dec_lastsample[ch] = audio[ch+(i<<1)];
		}
	}
}

void KillAudio()
{
	unsigned char *blk;
	int i,j,bn,N,ch;

	// kill the existing samples
	for (i=0;i < 10;i++) {
		for (j=0;j < 9;j++) {
			bn = (i * 150) + (j << 4) + 6;
			frame_blk[bn] = 0;
		}
	}
}

void Peaks()
{
	int i,v;

	audio_level = 0;
	for (i=0;i < (2*audio_len);i++) {
		v = abs(audio[i]);
		if (v > audio_level)
			audio_level = v;
	}
}

void ReverseAudio()
{
	int i,ch;

	for (i=0;i < audio_len;i++) {
		signed short *s1 = audio + (i << 1);
		signed short *s2 = audio + ((audio_len - (i + 1)) << 1);

		if (s1 >= s2)
			break;

		signed short tmp;
		tmp   = *s1;
		*s1++ = *s2;
		*s2++ = tmp;

		tmp   = *s1;
		*s1++ = *s2;
		*s2++ = tmp;
	}
}

static unsigned char *dv_decode_frame = NULL;
static unsigned char *dv_fbdev = NULL;
static unsigned char *dv_decode_to[576];
static int dv_pitches[576];

static int fbwidth = 1024;
static int fbheight = 768;

static struct termios old_ios;

int main(int argc,char **argv)
{
	DIFchunk *c;
	dv_decoder_t *dvcodec;
	int fd,newframe=0,prevseq=0,ofd,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;
	unsigned long current_alen=0;
	int fb_offset = 0;
	int fbx=0,fby=0;
	int fbfd = -1;
	int fbhalf = 0;

	if (argc < 3) {
		fprintf(stderr,"dvaudiotail [options] <file> <output file/device>\n");
		fprintf(stderr,"  -o offset (in frames)\n");
		return 1;
	}

	for (i=0;i < 576;i++) {
		dv_pitches[i] = 720*4;
		dv_decode_to[i] = NULL;
	}

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

				if (!strcmp(sw,"o")) {
					offset =
						((unsigned long long)strtoul(argv[i++],NULL,0)) *
						((unsigned long long)12000) *
						((unsigned long long)10);
				}
				else if (!strcmp(sw,"x")) {
					fbx = atoi(argv[i++]);
				}
				else if (!strcmp(sw,"y")) {
					fby = atoi(argv[i++]);
				}
				else if (!strcmp(sw,"half")) {
					fbhalf = 1;
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

	// set up video codec
	dvcodec = dv_decoder_new(TRUE, FALSE, FALSE);
	dvcodec->quality = DV_QUALITY_BEST;

	// so..... what are we opening exactly?
	char *ofd_id = NULL;
	{
		struct stat s;
		if (stat(outfile,&s) < 0) {
			fprintf(stderr,"Cannot stat %s\n",outfile);
			return 1;
		}

		if (S_ISCHR(s.st_mode)) {
			int major = (int)((s.st_rdev >> 8) & 0xFF);
			int minor = (int)((s.st_rdev     ) & 0xFF);

			// is this /dev/dsp OSS audio output?
			if (major == 14 && minor == 3) {
				fprintf(stderr,"Device looks like OSS device\n");
				ofd_id = "OSS";
			}
			else {
				fprintf(stderr,"I don't know this device. Failing!\n");
				return 1;
			}
		}
		else if (S_ISFIFO(s.st_mode)) {
			fprintf(stderr,"Ooooh! A FIFO!\n");
		}
		else if (S_ISSOCK(s.st_mode)) {
			fprintf(stderr,"Ooooh! A socket!\n");
		}
		else if (S_ISBLK(s.st_mode)) {
			fprintf(stderr,"Eewwww! A block device?!?\n");
			fprintf(stderr,"No way!\n");
			return 1;
		}
		else if (S_ISDIR(s.st_mode)) {
			fprintf(stderr,"%s is a directory you dumbass\n",outfile);
			return 1;
		}
	}

	fbfd = open("/dev/fb0",O_RDWR);
	if (fbfd < 0) {
		fprintf(stderr,"Cannot open framebuffer\n");
	}
	else {
		fb_offset = fbwidth * fby * 4;
		fb_offset += fbx * 4;
		
		dv_fbdev = mmap(NULL,fbwidth*fbheight*4,PROT_READ|PROT_WRITE,MAP_SHARED,fbfd,0);
		dv_decode_frame = malloc(720*576*4);

		if (dv_fbdev != NULL) {
			if (fbhalf) {
				int ly = 0;
				for (i=0;i < 576;i += 2) {
					dv_pitches[i] = 720 * 4;
					dv_decode_to[i] = dv_decode_frame + ((ly++) * 720 * 4);
				}
				for (i=1;i < 576;i += 2) {
					dv_pitches[i] = 720 * 4;
					dv_decode_to[i] = dv_decode_frame + ((ly++) * 720 * 4);
				}
			}
			else {
				for (i=0;i < 576;i++) {
					dv_pitches[i] = 720 * 4;
					dv_decode_to[i] = dv_decode_frame + (i * 720 * 4);
				}
			}
		}
	}

	ofd = open64(outfile,O_WRONLY);
	if (ofd < 0) {
		fprintf(stderr,"Cannot create/open file\n");
		return 1;
	}

	if (ofd_id != NULL) {
		if (!strcmp(ofd_id,"OSS")) {
			int x;
#define SNDCTLMANTRA(z,v) \
	x = v;\
	if (ioctl(ofd,z,&x) < 0) \
		fprintf(stderr,#z ": %s\n",strerror(errno));

			SNDCTLMANTRA(SNDCTL_DSP_CHANNELS,2);
			SNDCTLMANTRA(SNDCTL_DSP_SETFMT,AFMT_S16_LE);
			SNDCTLMANTRA(SNDCTL_DSP_SPEED,48000);
		}
	}

	memset(frame,0xFF,12000*10);
	memset(frame_blk,0,150*10);
	audio_len = 1600;

	// tell the terminal to lighten up and let us read in keyboard events
	if (isatty(0)) {
		struct termios nios;
		tcgetattr(0,&old_ios);
		memcpy(&nios,&old_ios,sizeof(struct termios));
		nios.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL);
		// disable canocial mode
		tcsetattr(0,TCSANOW,&nios);
	}

	/* cool. now scan the stream for other header blocks and pick out time codes */
	{
		fd = open64(dvfile,O_RDONLY | O_BINARY);
		if (fd < 0) return 1;

		unsigned long long file_size = lseek64(fd,0,SEEK_END);
		fprintf(stderr,"File size: %Lu\n",file_size);
		if (offset == -1) {
			offset = file_size / ((unsigned long long)(12000*10));
			offset *= ((unsigned long long)(12000*10));
		}
		fprintf(stderr,"Start @    %Lu\n",offset);

		int dv_code_countdown = 0;

		while (1) {
			// read user input
			{
				struct timeval tv;
				fd_set f;

				FD_ZERO(&f);
				FD_SET(0,&f);
				tv.tv_sec = 0;
				tv.tv_usec = 1000;
				int n = select(1,&f,NULL,NULL,&tv);
				if (n > 0) {
					unsigned char c=0;
					unsigned char osync=0;
					read(0,&c,1);

					// user input says what?
					if (c == 27)	// ESC = quit
						break;
					else if (c == ',') {	// , or <
						if (offset_adv >= 0)
							offset_adv = -1;
						else
							offset_adv -= 5;

						osync=1;
					}
					else if (c == '.') {	// . or >
						if (offset_adv <= 0)
							offset_adv = 1;
						else
							offset_adv += 5;

						osync=1;
					}
					else if (c == ' ') {	// spacebar
						offset_adv = 1;

						osync=1;
					}
					else if (c == 'n') {	// 'n' for NOW
						offset_adv = 1;
						offset = lseek64(fd,0,SEEK_END);
						offset -= offset % (12000*10);
						osync=1;
					}
					else if (c == 'z') {
						offset = 0;
						offset_adv = 1;
						osync=1;
					}
					else if (c == 'p') {
						offset_adv = 0;	// pause
						osync=1;
					}
					else if (c == '[') {
						if (offset > 120000)
							offset -= 120000;

						offset_adv = 0;
						osync=1;
					}
					else if (c == ']') {
						offset += 120000;
						offset_adv = 0;
						osync=1;
					}
					else if (c == '{') {
						if (offset > 1200000)
							offset -= 1200000;

						offset_adv = 0;
						osync=1;
					}
					else if (c == '}') {
						offset += 1200000;
						offset_adv = 0;
						osync=1;
					}

					if (osync) {
						if (!strcmp(ofd_id,"OSS"))
							ioctl(ofd,SNDCTL_DSP_RESET);
					}
				}
			}

			// how big is the file?
			unsigned long long cur_size = lseek64(fd,0,SEEK_END);
			if (offset >= cur_size && offset_adv > 1) {
				offset = cur_size;
				offset -= offset % (12000*10);
				offset_adv = 1;
				if (!strcmp(ofd_id,"OSS"))
					ioctl(ofd,SNDCTL_DSP_RESET);
			}
			else if (offset == 0 && offset_adv < 1) {
				offset = 0;
				offset_adv = 1;
				if (!strcmp(ofd_id,"OSS"))
					ioctl(ofd,SNDCTL_DSP_RESET);
			}

			// ugh okay so this is a cheap way to do it, but... it works
			// note that tracking is used here for those rare
			// weird cases where dvgrab gets things misaligned
			if (lseek64(fd,offset+(tracking*12000),SEEK_SET) != (offset+(tracking*12000)))
				continue;
			if (read(fd,cur_frame,12000*10) < (12000*10))
				continue;

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
					}
					else {
						printf("\nRaw DV tracking: Apparently whole frames are off by %d rows\nNothing done, only %d/10 conistent\n",offset,consistent);
					}
				}
			}

			// wait
			if (!strcmp(ofd_id,"OSS"))
				ioctl(ofd,SNDCTL_DSP_POST);

			// advance!
			if (offset_adv < 0) {
				if (offset < (-offset_adv * 12000 * 10))
					offset = -offset_adv * 12000 * 10;
			}
			offset += offset_adv * 12000 * 10;

			if (offset == poffset) continue;
			poffset = offset;

			int tH = -1,tM = -1,tS = -1,tF = -1;

			audio_len = 1600;
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
			}

			// de-interleave samples
			DecodeAudio();
			RemoveDamaged();
			KillAudio();
			Peaks();

			if (offset_adv < 0)
				ReverseAudio();

			if (isatty(1)) {
				// for the TTY make a pretty VU meter
				char line[72];
				int i,l,lred;

				l = (70 * audio_level) / 32767;
				lred = 64;
				fprintf(stdout,"\x0D" "\x1B[0m");

				if (tH >= 0)	fprintf(stdout,"%02u:",tH);
				else		fprintf(stdout,"xx:");
				if (tM >= 0)	fprintf(stdout,"%02u:",tM);
				else		fprintf(stdout,"xx:");
				if (tS >= 0)	fprintf(stdout,"%02u.",tS);
				else		fprintf(stdout,"xx.");
				if (tF >= 0)	fprintf(stdout,"%02u",tF);
				else		fprintf(stdout,"xx");
				
				for (i=0;i < lred;i++) {
					if (i < l)
						fprintf(stdout,"=");
					else
						fprintf(stdout," ");
				}
				fprintf(stdout,"\x1B[1;31m");
				for (   ;i < 70;i++) {
					if (i < l)
						fprintf(stdout,"=");
					else
						fprintf(stdout," ");
				}
				fprintf(stdout,"\x1B[0m");
				fflush(stdout);
			}
			else {
				fprintf(stdout,"\x0D" "%u",audio_level);
				fflush(stdout);
			}

			// drop samples if ahead
			{
				int adis = ((int)current_alen) - ((int)audio_shouldbe());
				if (adis > 256) {
					fprintf(stderr,"Output is ahead (%u > %u) dropping samples\n",current_alen,audio_shouldbe());
					audio_len -= 256;
				}
			}

			video_frames++;
			write(ofd,audio,audio_len*2*2);
			current_alen += audio_len;

			// fill in if behind
			int adis;
			do {
				adis = ((int)current_alen) - ((int)audio_shouldbe());
				if (adis < -256) {
					fprintf(stderr,"Output is behind (%u < %u) adding samples\n",current_alen,audio_shouldbe());
					memset(audio,0,1620*2*2);
					if (adis < -1600) adis = -1600;
					write(ofd,audio,-adis*2*2);
					current_alen += -adis;
				}
				else {
					break;
				}
			} while (1);

			// okay now decode video
			dv_code_countdown--;
			if (dv_code_countdown <= 0) {
				dv_parse_header(dvcodec,frame);
				dv_parse_packs(dvcodec,frame);
				dv_decode_full_frame(dvcodec,frame,e_dv_color_bgr0,dv_decode_to,dv_pitches);

				if (fbhalf) {
					int y,x;
					unsigned long *s,*d;

					for (y=0;y < 240;y++) {
						s = (unsigned long*)(dv_decode_frame + (y * 720 * 4 * 2));
						d = (unsigned long*)(dv_fbdev + fb_offset + (fbwidth * 4 * y));
						for (x=0;x < 360;x++) {
							*d++ = *s++;
							        s++;
						}
					}
				}
				else {
					int y,x;
					unsigned long *s,*d;

					for (y=0;y < 480;y++) {
						s = (unsigned long*)(dv_decode_frame + (y * 720 * 4));
						d = (unsigned long*)(dv_fbdev + fb_offset + (fbwidth * 4 * y));
						memcpy(d,s,720 * 4);
					}
				}
			}

			if (dv_code_countdown <= 0)
				dv_code_countdown = (offset_adv >= -2 && offset_adv <= 2 && offset_adv != 0) ? 2 : 0;
		}

		close(fd);
	}

	unsigned long final_len = lseek64(ofd,0,SEEK_END);
	if (isatty(0)) tcsetattr(0,TCSANOW,&old_ios);
	printf("\n");
	close(ofd);
	close(fd);
	return 0;
}

