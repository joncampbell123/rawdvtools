#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <linux/soundcard.h>	// OSS audio ioctls
#include <linux/fb.h>		// Linux framebuffer ioctls
#include <libdv/dv.h>		// libdv video decoder

#include <ffmpeg/avcodec.h>

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
int min_samples[4] = {1580,1452,1053,-1};
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
				unsigned char *data = blk+3;
				unsigned char more_samples = data[1] & 0x3F;
				unsigned char sidx = (data[4] >> 3) & 3;
				quant = quants[data[4] & 7];
				srate = srates[sidx];
				audio_len = min_samples[sidx] + more_samples;
			}
		}
	}

	if (quant < 0 || srate < 0 || audio_len <= 0)
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
		static int prev[2];
		signed short or[audio_len*2];
		int i=0,o=0,sc=0;

		memcpy(or,audio,audio_len*2*2);
		while (o < (1620*2) && i < (audio_len*2)) {
			audio[o++] =
				((or[i  ]*sc)+(prev[0]*(48000-sc)+24000))/
				48000;
			audio[o++] =
				((or[i+1]*sc)+(prev[1]*(48000-sc)+24000))/
				48000;
			sc += srate;
			if (sc >= 48000) {
				sc -= 48000;
				prev[0] = or[i  ];
				prev[1] = or[i+1];
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

static signed short audio_buffer[1620*30*2];
static int audio_buffer_x = 0;

void audio_buffer_add(signed short *x,int l) {
	if ((audio_buffer_x+l) > (1620*30)) {
		fprintf(stderr,"Audio buffer overflow\n");
		return;
	}

	memcpy(audio_buffer + (audio_buffer_x << 1),x,l * 2 * 2);
	audio_buffer_x += l;
}

void audio_buffer_remove(int x) {
	if (x >= audio_buffer_x) {
		audio_buffer_x = 0;
		return;
	}

	int rem = audio_buffer_x - x;
	memmove(audio_buffer,audio_buffer + (x << 1),rem * 2 * 2);
	audio_buffer_x = rem;
}

static unsigned char *dv_decode_frame = NULL;
static unsigned char *dv_decode_to[576];
static int dv_pitches[576];

AVCodec*                        mpv_codec = NULL;
AVCodecContext*                 mpv_context = NULL;
AVFrame*                        mpv_frame = NULL;
unsigned char                   mpv_temp[8*1024*1024];  /* in case one frame of MPEG-2 takes 8MB? */
AVCodec*                        ac3_codec = NULL;
AVCodecContext*                 ac3_context = NULL;
unsigned char                   ac3_temp[65536*32];

int MPVARHack(unsigned char *tmp,int code)
{
	if (*tmp++ != 0x00) return 0;
	if (*tmp++ != 0x00) return 0;
	if (*tmp++ != 0x01) return 0;
	if (*tmp++ != 0xB3) return 0;
	tmp++;
	tmp++;
	tmp++;
	*tmp &= 0x0F;
	*tmp |= code << 4;
	return 1;
}

unsigned long estimate(unsigned long long current,unsigned long long srclen,unsigned long long srctotal) {
	unsigned long long x = (current * srctotal) / srclen;
	x += 999LL;
	x /= 1000LL;
	return ((unsigned long)x);
}

unsigned long estimate_time(unsigned long long current,unsigned long long srclen,unsigned long long srctotal) {
	unsigned long long x = (current * srctotal) / srclen;
	return ((unsigned long)x);
}

unsigned long long mpv_counter = 0;
unsigned long long ac3_counter = 0;

/* what really annoys me is that consumer level camcorders have such faded colors */
void consumer_dv_chroma(unsigned char *chroma,int cw,int ch) {
	int x = cw * ch;
	while (x-- > 0) {
		int c = *chroma - 128;

		c = (c * 255) / 240;

		c += 128;
		if (c < 0) c = 0;
		else if (c > 255) c = 255;
		*chroma++ = (unsigned char)c;
	}
}

/* LibDV always returns YUY2 but if the source is 4:1:1 DV then you'll get these ugly chroma
 * stairsteps in the image and then MPEG compression sucks */
void libdv_411_filter(unsigned char *chroma,int cw,int ch) {
	int y,x;
	for (y=0;y < ch;y++) {
		unsigned char *p = chroma + (y * cw);
		for (x=0;x <= (cw-2);x += 2) {
			/* don't touch if either libdv is smoothing them out or the source isn't 4:1:1 */
			if (p[0] != p[1]) return;
			p[1] = (p[0] + p[2] + 1) >> 1;
			p += 2;
		}
	}
}

/* my camcorder clamps to 16....235 so bright scenes look fake and superficial */
void consumer_dv_luma(unsigned char *luma,int w,int h) {
	int x = w * h;
	while (x-- > 0) {
		int l = *luma - 16;

		l = (l * 255) / 235;

		l += 16;
		if (l < 0) l = 0;
		else if (l > 255) l = 255;
		*luma++ = (unsigned char)l;
	}
}

/* most consumer camcorders apparently have only enough CCD
 * to effectively capture 360x240 or 360x480. They get 720x480
 * by cheap interpolation and then using cheap sharpening
 * to hide it. The sharpening is obvious on sharp edges as a
 * "zebra stripe" pattern, which doesn't help MPEG compression any.
 * We filter horizontally, to remove the oversharping "zebra pattern" */

void consumer_dv_luma_360x_median(unsigned char *luma,int w,int h) {
	unsigned char *row,nr[720];
	int y,x;

	for (y=0;y < h;y++) {
		row = luma + (y * 720);
		nr[0] = row[0];
		for (x=1;x < w;x++) nr[x] = (row[x-1] + row[x] + 1) >> 1;
		memcpy(row,nr,720);
	}
}

int main(int argc,char **argv)
{
	DIFchunk *c;
	dv_decoder_t *dvcodec;
	int fd,newframe=0,prevseq=0,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;
	unsigned long current_alen=0;
	int fb_offset = 0;
	int fbx=0,fby=0;
	int fbfd = -1;
	int fbhalf = 0;

	if (argc < 3) {
		fprintf(stderr,"dv2mpeg [options] <file> <base>\n");
		fprintf(stderr,"  -o offset (in frames)\n");
		return 1;
	}

	for (i=0;i < 576;i++) {
		dv_pitches[i] = 720*2;
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

	// set up video codec
	dvcodec = dv_decoder_new(TRUE, FALSE, FALSE);
	dvcodec->quality = DV_QUALITY_BEST;

	dv_decode_frame = malloc(720*576*2);

	{
		int y,x;
		for (y=0;y < 480;y++) {
			for (x=0;x < 720;x += 2) {
				int o = (y * 720 * 2) + (x * 2);
				dv_decode_frame[o++] = 16;
				dv_decode_frame[o++] = 128;
				dv_decode_frame[o++] = 16;
				dv_decode_frame[o++] = 128;
			}
		}
	}

	for (i=0;i < 576;i++) {
		dv_pitches[i] = 720 * 2;
		dv_decode_to[i] = dv_decode_frame + (i * 720 * 2);
	}

	memset(frame,0xFF,12000*10);
	memset(frame_blk,0,150*10);
	audio_len = 1600;

	// set up FFMPEG codecs
	avcodec_init();
	avcodec_register_all();

	mpv_codec = avcodec_find_encoder(CODEC_ID_MPEG2VIDEO);
	if (!mpv_codec) {
		printf("ffmpeg does not have FLV video encoder\n");
		return 1;
	}

	mpv_context = avcodec_alloc_context();
	if (!mpv_context) {
		printf("ffmpeg cannot alloc context\n");
		return 1;
	}

	ac3_codec = avcodec_find_encoder(CODEC_ID_AC3);
	if (!ac3_codec) {
		printf("ffmpeg does not have AC-3 encoder\n");
		return 1;
	}

	ac3_context = avcodec_alloc_context();
	if (!ac3_context) {
		printf("cannot alloc AC-3 context\n");
		return 1;
	}

	mpv_frame = avcodec_alloc_frame();
	if (!mpv_frame) {
		printf("ffmpeg cannot alloc frame\n");
		return 1;
	}

	int mpv_width = 720;
	int mpv_height = 480;

	mpv_context->bit_rate = 6250000;
	mpv_context->width = mpv_width;
	mpv_context->height = mpv_height;
	mpv_context->time_base = (AVRational){1001,30000};
	mpv_context->gop_size = 30;
	mpv_context->max_b_frames = 2;
	mpv_context->noise_reduction = 0;
	mpv_context->spatial_cplx_masking = 0.0;
	mpv_context->temporal_cplx_masking = 0.0;
	mpv_context->p_masking = 0.0;
	mpv_context->rc_min_rate = mpv_context->bit_rate;
	mpv_context->rc_max_rate = mpv_context->bit_rate;
	mpv_context->rc_buffer_size = 1835000;
	mpv_context->pix_fmt = PIX_FMT_YUV420P;
	mpv_context->me_range = 0;
	mpv_context->dia_size = -3;
	mpv_context->me_method = ME_EPZS;
	mpv_context->flags = CODEC_FLAG_INTERLACED_ME | CODEC_FLAG_INTERLACED_DCT | CODEC_FLAG_ALT_SCAN | CODEC_FLAG_CLOSED_GOP;
	mpv_context->flags2 = 0;
	mpv_context->scenechange_threshold = 10000000000LL;
	mpv_context->qmin = 2;
	mpv_context->qmax = 23;
	mpv_context->rc_eq = "1";
	mpv_frame->top_field_first = 0;
	mpv_frame->interlaced_frame = 1;

	if (avcodec_open(mpv_context, mpv_codec) < 0) {
		printf("cannot open MPEG-2 codec\n");
		return 1;
	}

	int audio_bitrate = 224000;
	ac3_context->bit_rate = audio_bitrate;
	ac3_context->sample_rate = 48000;
	ac3_context->channels = 2;
	ac3_context->sample_fmt = SAMPLE_FMT_S16;

	if (avcodec_open(ac3_context, ac3_codec) < 0) {
		printf("Cannot AC-3 codec\n");
		return 1;
	}

	unsigned char *yuv = (unsigned char*)malloc(mpv_width * mpv_height * 2);
	mpv_frame->data[0] = yuv;
	mpv_frame->data[1] = yuv + (mpv_width * mpv_height);
	mpv_frame->data[2] = yuv + (mpv_width * mpv_height) + ((mpv_width>>1) * (mpv_height>>1));
	mpv_frame->linesize[0] = mpv_width;
	mpv_frame->linesize[1] = mpv_width >> 1;
	mpv_frame->linesize[2] = mpv_width >> 1;

	char mpv_file[256];
	sprintf(mpv_file,"%s.m2v",outfile);
	int mpv_fd = open64(mpv_file,O_CREAT|O_TRUNC|O_RDWR,0644);
	if (mpv_fd < 0) {
		fprintf(stderr,"Cannot open %s\n",mpv_file);
		return 1;
	}

	char ac3_file[256];
	sprintf(ac3_file,"%s.ac3",outfile);
	int ac3_fd = open64(ac3_file,O_CREAT|O_TRUNC|O_RDWR,0644);
	if (ac3_fd < 0) {
		fprintf(stderr,"Cannot open %s\n",ac3_file);
		return 1;
	}

	unsigned long mpv_last_bits = 0;
	unsigned long ac3_last_bits = 0;
	unsigned long mpv_avg_bits = 0;
	unsigned long ac3_avg_bits = 0;

	int frames = 0;
	/* cool. now scan the stream for other header blocks and pick out time codes */
	{
		fd = open64(dvfile,O_RDONLY | O_BINARY);
		if (fd < 0) return 1;

		unsigned long long file_size = lseek64(fd,0,SEEK_END);
		offset = 0;

		time_t began = time(NULL);
		while (1) {
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

			// drop samples if ahead
			{
				int adis = ((int)current_alen) - ((int)audio_shouldbe());
				if (adis > 256) {
					fprintf(stderr,"Output is ahead (%u > %u) dropping samples\n",current_alen,audio_shouldbe());
					audio_len -= 256;
				}
			}

			video_frames++;
			audio_buffer_add(audio,audio_len);
			current_alen += audio_len;

			// fill in if behind
			int adis;
			do {
				adis = ((int)current_alen) - ((int)audio_shouldbe());
				if (adis < -256) {
					fprintf(stderr,"Output is behind (%u < %u) adding samples\n",current_alen,audio_shouldbe());
					memset(audio,0,1620*2*2);
					if (adis < -1620) adis = -1620;
					audio_buffer_add(audio,-adis);
					current_alen += -adis;
				}
				else {
					break;
				}
			} while (1);

			// okay now decode video
			dv_parse_header(dvcodec,frame);
			dv_parse_packs(dvcodec,frame);
			dv_decode_full_frame(dvcodec,frame,e_dv_color_yuv,dv_decode_to,dv_pitches);

			/* encode video. we have to also convert YUY2 -> YV12 */
			{
				unsigned char *i,*Y,*U,*V;
				int x,y;

				if (0) {
					/* progressive */
					for (y=0;y < 480;y += 2) {
						i = dv_decode_to[y];
						Y = mpv_frame->data[0] + (y * 720);
						U = mpv_frame->data[1] + ((y>>1) * 360);
						V = mpv_frame->data[2] + ((y>>1) * 360);
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*U++ = i[1];
							*Y++ = i[2];
							*V++ = i[3];
							i += 4;
						}

						i = dv_decode_to[y+1];
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*Y++ = i[2];
							i += 4;
						}
					}
				}
				else {
					/* interlaced */
					for (y=0;y < 480;y += 4) {
						i = dv_decode_to[y];
						Y = mpv_frame->data[0] + (y * 720);
						U = mpv_frame->data[1] + ((y>>1) * 360);
						V = mpv_frame->data[2] + ((y>>1) * 360);
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*U++ = i[1];
							*Y++ = i[2];
							*V++ = i[3];
							i += 4;
						}

						i = dv_decode_to[y+1];
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*U++ = i[1];
							*Y++ = i[2];
							*V++ = i[3];
							i += 4;
						}

						i = dv_decode_to[y+2];
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*Y++ = i[2];
							i += 4;
						}

						i = dv_decode_to[y+3];
						for (x=0;x < 720;x += 2) {
							*Y++ = i[0];
							*Y++ = i[2];
							i += 4;
						}
					}
				}

				consumer_dv_chroma(mpv_frame->data[1],720>>1,480>>1);
				consumer_dv_chroma(mpv_frame->data[2],720>>1,480>>1);
//				consumer_dv_luma(mpv_frame->data[0],720,480);
				consumer_dv_luma_360x_median(mpv_frame->data[0],720,480);
				libdv_411_filter(mpv_frame->data[1],720>>1,480>>1);
				libdv_411_filter(mpv_frame->data[2],720>>1,480>>1);

				if ((frames % mpv_context->gop_size) == 0)
					mpv_frame->pict_type = FF_I_TYPE;
				else
					mpv_frame->pict_type = ((frames - 1) % mpv_context->max_b_frames) == 0 ? FF_P_TYPE : FF_B_TYPE;

				int tmp = avcodec_encode_video(mpv_context, mpv_temp, 4*1024*1024, mpv_frame);
				if (tmp > 0) {
					MPVARHack(mpv_temp,3);	/* 2=4:3  3=16:9 */
					write(mpv_fd,mpv_temp,tmp);
					mpv_counter += tmp;
					mpv_last_bits = tmp << 3;
				}
				else {
					mpv_last_bits = 0;
					printf("video encoder error?\n");
				}
			}

			/* encode audio */
			while (audio_buffer_x >= 1536) {
				signed short *x = audio_buffer;
				int tmp = avcodec_encode_audio(ac3_context,ac3_temp,65536,audio_buffer);
				if (tmp > 0) {
					write(ac3_fd,ac3_temp,tmp);
					ac3_counter += tmp;
					ac3_last_bits = tmp << 3;
				}
				else {
					ac3_last_bits = 0;
					printf("audio encoder error?\n");
				}
				audio_buffer_remove(1536);
			}

			/* advance */
			offset += 12000*10;
			frames++;

			if (isatty(1)) {
				// for the TTY make a pretty VU meter
				char line[72];
				int i,l,lred;

				l = (32 * audio_level) / 32767;
				lred = 28;
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
				for (   ;i < 32;i++) {
					if (i < l)
						fprintf(stdout,"=");
					else
						fprintf(stdout," ");
				}
				fprintf(stdout,"\x1B[0m");

				mpv_avg_bits = ((mpv_avg_bits * 15) + mpv_last_bits + 8) >> 4;
				ac3_avg_bits = ((ac3_avg_bits * 15) + ac3_last_bits + 8) >> 4;

				fprintf(stdout,"[%dv:%dKB @ %dv:%d kb/sec] be [%dv:%dKB]",
						(unsigned long)((mpv_counter+999)/1000),
						(unsigned long)((ac3_counter+999)/1000),
						((mpv_avg_bits*30)+999)/1000,
						((ac3_avg_bits*30)+999)/1000,
						(unsigned long)estimate(mpv_counter,offset,file_size),
						(unsigned long)estimate(ac3_counter,offset,file_size));

				if (mpv_avg_bits >= (8000000)) {
					fprintf(stdout,"\n\x07HEY! FFMPEG is VIOLATING BITRATE CONSTRAINTS!!!!\n");
				}

				time_t cur = time(NULL);
				if (cur != began) {
					time_t pred = estimate_time(cur-began,offset,file_size);
					pred -= (cur - began);
					if (pred >= 3600) {
						fprintf(stdout," in %d hours %d min",pred/3600,(pred/60)%60);
					}
					else if (pred >= 60) {
						fprintf(stdout," in %d min %d sec",pred/60,pred%60);
					}
					else if (pred > 0) {
						fprintf(stdout," in %d sec",pred);
					}
				}

				fprintf(stdout,"    ");
				fflush(stdout);
			}
		}
	}

	close(mpv_fd);
	close(ac3_fd);
	close(fd);
	return 0;
}

