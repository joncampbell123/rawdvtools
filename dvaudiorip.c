#define _FILE_OFFSET_BITS 64

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct {
	unsigned char		raw[80];
	unsigned char		SCT,Arb,Dseq,FSC,DBN;	// decoded from DIF header
} DIFchunk;

static DIFchunk ReadDIFtmp;

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

	lseek(fd,0,SEEK_SET);
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

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=0,prevseq=0,ofd,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;
	unsigned long current_alen=0;

	if (argc < 3) {
		fprintf(stderr,"dvdumptimecodes <file [file ...]> <.WAV>\n");
		return 1;
	}

	ofd = open(argv[argc-1],O_RDWR | O_TRUNC | O_CREAT,0644);
	if (ofd < 0) {
		fprintf(stderr,"Cannot create file\n");
		return 1;
	}

	WriteWAV(ofd,0);
	memset(frame,0xFF,12000*10);
	memset(frame_blk,0,150*10);
	audio_len = 1600;

	/* cool. now scan the stream for other header blocks and pick out time codes */
	for (pass=1;pass < argc-1;pass++) {
		fd = open(argv[pass],O_RDONLY | O_BINARY);
		if (fd < 0) continue;

		lseek(fd,0,SEEK_SET);
		while (read(fd,cur_frame,12000*10) > 0) {
			audio_len = 1600;
			memset(frame_blk,0,sizeof(frame_blk));
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
			}

			// de-interleave samples
			DecodeAudio();
			RemoveDamaged();
			KillAudio();

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
		}

		close(fd);
	}

	unsigned long final_len = lseek(ofd,0,SEEK_END);
	WriteWAV(ofd,final_len - 44);
	close(ofd);
	close(fd);
	return 0;
}

