#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

unsigned long long lseek64(int fd,unsigned long long x,int whence);

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

void FixBadSamples(DIFchunk *c)
{
	unsigned short *x = (unsigned short*)(c->raw + 8);
	int i=0,s;

	while (i < 36 && x[i] == 0x0080)
		i++;

	if (i == 36)
		s = 0;

	i = 0;
	while (i < 36) {
		if (x[i] == 0x0080)	x[i] = s;
		else			s = x[i];
		i++;
	}
}

void PatchAudioBlocks(unsigned char *frame,unsigned char *blkmap)
{
	int i,j;

	for (i=0;i < 10;i++) {
		for (j=0;j < 9;j++) {
			int bn = (i * 150) + (j << 4) + 6;
			unsigned char *blk = frame + (bn * 80);

			blk[0] = 0x7F;
			blk[1] = (i << 4) | 7;
			blk[2] = j;

			unsigned char *FC = blk + 3;
			switch (j) {
				case 0:
					FC[0] = 0x50;
					FC[1] = 0xD5;
					FC[2] = 0x00;
					FC[3] = 0xC0;
					FC[4] = 0xC0;
					break;
				case 1:
					FC[0] = 0x51;
					FC[1] = 0x0F;
					FC[2] = 0xCF;
					FC[3] = 0xA0;
					FC[4] = 0xFF;
					break;
				case 2:
//					FC[0] = 0x52;
//					FC[1] = 0xD6;
//					FC[2] = 0xC4;
//					FC[3] = 0xE7;
//					FC[4] = 0x06;
					break;
				case 3:
//					FC[0] = 0x53;
//					FC[1] = 0xFF;
//					FC[2] = 0xB2;
//					FC[3] = 0xA7;
//					FC[4] = 0xD7;
					break;
				default:
					FC[0] = FC[1] = FC[2] = FC[3] = FC[4] = 0xFF;
					break;
			};

			if (!blkmap[bn]) {
				unsigned char *a = blk + 8;
				memset(a,0,72);
			}
		}
	}
}

static char *truefalse[2] = {"false","true"};
static unsigned char cur_frame[12000*12];
static unsigned char frame[12000*12],frame_blk[150*12];

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=0,prevseq=0,ofd,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;

	if (argc < 3) {
		fprintf(stderr,"dvdumptimecodes <file> <out>\n");
		return 1;
	}

/* a DV file is likely > 2GB so use open64 */
	fd = open64(argv[1],O_RDONLY | O_BINARY);
	if (fd < 0) {
		fprintf(stderr,"Cannot open file for reading\n");
		return 1;
	}

	ofd = open64(argv[2],O_WRONLY | O_TRUNC | O_CREAT,0644);
	if (ofd < 0) {
		fprintf(stderr,"Cannot create file\n");
		return 1;
	}

	memset(frame,0xFF,12000*10);
	memset(frame_blk,0,150*10);
	PatchAudioBlocks(frame,frame_blk);

	/* cool. now scan the stream for other header blocks and pick out time codes */
	for (pass=0;pass < 1;pass++) {
		lseek64(fd,0,SEEK_SET);
		while (read(fd,cur_frame,12000*10) > 0) {
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
					if (c->SCT == 3) FixBadSamples(c);
					memcpy(frame + (bo * 80),c->raw,80);
					frame_blk[bo] = 1;
				}
			}

			PatchAudioBlocks(frame,frame_blk);
			if (pass == 0) write(ofd,frame,12000*10);
		}
	}

	close(ofd);
	close(fd);
	return 0;
}

