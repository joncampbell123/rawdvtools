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

static char *truefalse[2] = {"false","true"};
static unsigned char frame[12000*12];

int main(int argc,char **argv)
{
	DIFchunk *c;
	int fd,newframe=0,prevseq=0,ofd,pass=0,i,j;
	int patchH=0,patchM=0,patchS=0,patchF=-1,asx=0;

	if (argc < 4) {
		fprintf(stderr,"dvforceaspect <in> <out> <aspect>\n");
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

	int aspect_code = 0;
	if (!strcmp(argv[3],"16:9"))
		aspect_code = 2;
	else
		aspect_code = 0;

	printf("Patching stream to indicate %s\n",
		aspect_code == 2 ? "16:9" : "4:3");

	/* cool. now scan the stream for other header blocks and pick out time codes */
	lseek64(fd,0,SEEK_SET);
	while (read(fd,frame,12000*10) > 0) {
		unsigned char *VAUX[3];
		VAUX[0] = VAUX[1] = VAUX[2] = NULL;
		unsigned char *first_empty = NULL;
		unsigned char *VAUXSC = NULL;
		unsigned char *VAUXSC_empty = NULL;
		int first_empty_N = 0;

		for (asx=0;asx < (150*10);asx++) {
			DIFchunk *c = PickDIF(frame + (asx * 80));
			if (!c) continue;

			if (c->raw[0] == 0 && c->raw[1] == 0 && c->raw[2] == 0 && first_empty == NULL)
				if (first_empty_N < 3)
					first_empty[first_empty_N++] = frame + (asx * 80);

			/* we concern ourselves with the VAUX packets */
			if (c->SCT == 2) {
				if (c->DBN < 3) {
					unsigned char *C = frame + (asx * 80);
					if (!VAUX[c->DBN]) VAUX[c->DBN] = C;

					unsigned char *PC = C + 3;
					unsigned char *fence = C + 78;

					while (PC < fence) {
						if (PC[0] == 0x61) {
							/* convert to 16:9 */
							PC[2] &= ~7;
							PC[2] |=  2;
							VAUXSC = PC;
						}

						PC += 5;
					}
				}
			}
		}

		/* if no VAUX packets were found PERIOD, make one */
		if (VAUX[0] == NULL && VAUX[1] == NULL && VAUX[2] == NULL) {
			printf("WARNING: Frame is missing VAUX packets, generating\n");
			VAUX[0] = frame + (3 * 80);
			VAUX[1] = frame + (4 * 80);
			VAUX[2] = frame + (5 * 80);
		}

		if (VAUX[0][0] == 0) {
			VAUX[0][0] = 0x51;
			VAUX[0][1] = 0x07 | ((asx / 150) << 4);
			VAUX[0][2] = 0x00;
		}
		if (VAUX[1][0] == 0) {
			VAUX[1][0] = 0x51;
			VAUX[1][1] = 0x07 | ((asx / 150) << 4);
			VAUX[1][2] = 0x01;
		}
		if (VAUX[2][0] == 0) {
			VAUX[2][0] = 0x51;
			VAUX[2][1] = 0x07 | ((asx / 150) << 4);
			VAUX[2][2] = 0x02;
		}

		/* if no VAUX source control was found, see if there is vacant space in the VAUX packets to make one */
		if (VAUXSC == NULL) {
			printf("WARNING: No VAUX source control packet. Creating one\n");
			VAUXSC = VAUX[2] + 53;	/* fixed place, because QuickTime and Final Cut Pro demand it */
			VAUXSC[0] = 0x61;
			VAUXSC[1] = 0x3F;
			VAUXSC[2] = 0xC8 | 2;
			VAUXSC[3] = 0xFC;
			VAUXSC[4] = 0xFF;
		}

		if (VAUXSC == NULL)
			fprintf(stderr,"WARNING: Frame does not have room or capability of VAUX SRC packet\n");

		write(ofd,frame,12000*10);
	}

	close(ofd);
	close(fd);
	return 0;
}

