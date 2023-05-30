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

DIFchunk *ReadDIF(int fd)
{
	DIFchunk *d = &ReadDIFtmp;

	if (read(fd,d->raw,80) < 80)
		return NULL;

	/* pick apart DIF header */
	d->SCT =   d->raw[0] >> 5;
	d->Arb =   d->raw[0]       & 0xF;
	d->Dseq =  d->raw[1] >> 4;
	d->FSC =  (d->raw[1] >> 3) & 1;
	d->DBN =   d->raw[2];
	return d;
}

static char *truefalse[2] = {"false","true"};

static unsigned char frame[12000 * 12];	// for up to 12 sequence blocks
static unsigned char framemapped[150 * 12];
static unsigned char framecontent=0;
static char isPAL=0;
static int audiocode[2] = {-1,-1};
static int out_fd = -1;
static int out_seq = 0;

void clear()
{
	memset(framemapped,0,150 * 12);
	memset(frame,0,12000 * 12);
	framecontent=0;
}

int writeframe()
{
	int i,packs,j,acode[2] = {-1,-1};
	char change=0;
	char hasaudio=0;
	char PAL=0;

	if (!framecontent)
		return 0;

// fill in missing header packets. if we don't have
// any headers then don't write it.
	for (j=0;j < 12 && !framemapped[j * 150];) j++;
	if (j >= 12) return 0;

	for (i=0;i < 12;i++) {
		if (!framemapped[i * 150]) {
			memcpy(&frame[i * 12000],&frame[j * 12000],80);
			framemapped[i * 150]++;
		}
	}

	PAL = (char)(frame[0] & 0x80);
	if (PAL != isPAL) change = 1;

// did the audio sample rate change?
	for (i=0;i < 12;i++) {
		for (j=0;j < 9;j++) {
			int bn = (i * 150) + ((j << 4) + 6);
			unsigned char *blk = frame + (bn * 80);
			unsigned char *PC = blk + 3;
			int nch = 0;

			if (!framemapped[bn])		// must have been there
				continue;

			if ((blk[0] >> 5) != 3)		// must be an audio block
				continue;

			hasaudio=1;
			if (PC[0] != 0x50)		// must be a specific AAUX chunk
				continue;

			nch = PC[2] & 0xF;
			if (nch >= 2)
				continue;

			acode[nch] = (PC[4] >> 3) & 7;
		}
	}

// todo: synthesize audio blocks, using silence or previous frame's audio

	for (i=0;i < 2;i++) {
		if (acode[i] != audiocode[i]) {
			audiocode[i] = acode[i];
			change = 1;
		}
	}

	if (change) {
		close(out_fd);
		out_fd = -1;
	}

	if (out_fd < 0) {
		char name[64];
		char desc[64];

		desc[0]=0;
		if (PAL) strcat(desc,"PAL-");
		else strcat(desc,"NTSC-");

		if (audiocode[0] == 0) strcat(desc,"48KHz");
		else if (audiocode[0] == 2) strcat(desc,"32KHz");
		else sprintf(desc+strlen(desc),"acode%d",audiocode[0]);
		
		sprintf(name,"split-output-%04u-%s.dv",out_seq++,desc);
		printf("writing to: %s\n",name);
		out_fd = open(name,O_WRONLY | O_TRUNC | O_CREAT | O_BINARY,0644);
		if (out_fd < 0) {
			fprintf(stderr,"Unable to create %s\n",name);
			return 0;
		}
	}

	write(out_fd,frame,12000 * (isPAL ? 12 : 10));
	isPAL = PAL;
	return 1;
}

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=1,prevseq=0,ofd;
	int patchH=0,patchM=0,patchS=0,patchF=-1;

	if (argc < 2) {
		fprintf(stderr,"dvdumptimecodes <file> <out>\n");
		return 1;
	}

/* a DV file is likely > 2GB so use open */
	fd = open(argv[1],O_RDONLY | O_BINARY);
	if (fd < 0) {
		fprintf(stderr,"Cannot open file for reading\n");
		return 1;
	}

	/* blah blah blah */
//	printf("Found DV header @ %Lu\n",lseek(fd,0,SEEK_CUR) - ((unsigned long long)80));
//	printf("System: %s\n",(c->raw[3] >> 7) ? "625/50 PAL system" : "525/60 NTSC system");
//	printf("Audio DIF blocks present:   %s\n",truefalse[(c->raw[3] >> 5) & 1]);
//	printf("Video AUX blocks present:   %s\n",truefalse[(c->raw[3] >> 4) & 1]);
//	printf("Subcode DIF blocks present: %s\n",truefalse[(c->raw[3] >> 3) & 1]);

	/* good. scan the stream, pick out blocks for each frame and reassemble what we can, fill in what we cant. */
	clear();
	while ((c = ReadDIF(fd)) != NULL) {
		int difseq = 0;
		unsigned char *blk;
		if (c->Dseq >= 12)
			continue;

		/* new frame? */
		if (c->SCT == 0 && c->Dseq == 0 && c->FSC == 0 && c->DBN == 0) {
			writeframe();
			clear();
		}

		if (c->SCT == 0) {
			if (c->DBN != 0)
				continue;
		}
		else if (c->SCT == 1) {
			difseq = 1 + c->DBN;
		}
		else if (c->SCT == 2) {
			difseq = 3 + c->DBN;
		}
		else if (c->SCT == 3) {
			difseq = 6 + (c->DBN * 16);
		}
		else if (c->SCT == 4) {
			difseq = 7 + (c->DBN / 15) + c->DBN;
		}
		else {
			continue;
		}

		if (difseq >= 150)
			continue;

		blk = frame + (c->Dseq * 12000) + (difseq * 80);
		if (framemapped[(c->Dseq * 150) + difseq]++ == 0) memcpy(blk,c->raw,80);
		framecontent=1;
	}
	writeframe();

	if (out_fd >= 0)
		close(out_fd);

	close(fd);
	return 0;
}

