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

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=1,prevseq=0;

	if (argc < 2) {
		fprintf(stderr,"dvdumptimecodes <file>\n");
		return 1;
	}

/* a DV file is likely > 2GB so use open64 */
	fd = open64(argv[1],O_RDONLY | O_BINARY);
	if (fd < 0) {
		fprintf(stderr,"Cannot open file for reading\n");
		return 1;
	}

	/* read first DIF, figure out the format of the stream. */
	/* Do not exit this loop until we have the first DIF header chunk, first block, first sequence */
	do {
		if (!(c = ReadDIF(fd))) {
			fprintf(stderr,"Unable to locate a header DIF chunk\n");
			return 1;
		}
	} while (c->SCT != 0 || c->Dseq != 0 || c->DBN != 0 || c->FSC != 0);

	/* blah blah blah */
	printf("Found DV header @ %Lu\n",lseek64(fd,0,SEEK_CUR) - ((unsigned long long)80));
	printf("System: %s\n",(c->raw[3] >> 7) ? "625/50 PAL system" : "525/60 NTSC system");
	printf("Audio DIF blocks present:   %s\n",truefalse[(c->raw[3] >> 5) & 1]);
	printf("Video AUX blocks present:   %s\n",truefalse[(c->raw[3] >> 4) & 1]);
	printf("Subcode DIF blocks present: %s\n",truefalse[(c->raw[3] >> 3) & 1]);

	/* cool. now scan the stream for other header blocks and pick out time codes */
	while ((c = ReadDIF(fd)) != NULL) {
		if (c->SCT == 0) {
			/* there are 10 or 12 blocks per frame, so it's only a new frame if Dseq == 0. */
			if (c->Dseq <= prevseq) {
				if (newframe == 1) {
					// we didn't hit a timecode
					printf("Frame has no timecode\n");
				}
				newframe=1;
			}
			prevseq = c->Dseq;
		}
		if (c->SCT == 1) {
			unsigned char *SSYB,Syb,*PC;
			int SN;
			
			/* scan the subcode SSYBs */
			for (SN=0;SN < 8;SN++) {
				SSYB = c->raw + 3 + (SN * 8);
				if (SSYB[2] != 0xFF) continue;
				PC = SSYB + 3;

				Syb = SSYB[1] & 0xF;
				if ((Syb == 3 || Syb == 9) && PC[0] == 0x13) {
					if (--newframe == 0) {
						int H,M,S,F;

						/* sanity checks. the binary values are effectivly BCD, so: */
						if (	(PC[4] & 0xF) >= 10 ||
							(PC[3] & 0xF) >= 10 ||
							(PC[2] & 0xF) >= 10 ||
							(PC[1] & 0xF) >= 10)
							continue;

						H = (PC[4] & 0xF) + (((PC[4] >> 4) & 3) * 10);
						M = (PC[3] & 0xF) + (((PC[3] >> 4) & 7) * 10);
						S = (PC[2] & 0xF) + (((PC[2] >> 4) & 7) * 10);
						F = (PC[1] & 0xF) + (((PC[1] >> 4) & 3) * 10);

						printf("%u:%02u:%02u.%02u\n",H,M,S,F);
					}
				}
			}
		}
	}

	close(fd);
	return 0;
}

