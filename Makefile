all: dvdumptimecodes dvmassagetime dvsplit dvrepfill dvaudiorip dvaudiotail dvforceaspect dv2mpeg dv24pa-qt

dvdumptimecodes: dvdumptimecodes.c
	gcc -o dvdumptimecodes dvdumptimecodes.c

dvmassagetime: dvmassagetime.c
	gcc -o dvmassagetime dvmassagetime.c

dvforceaspect: dvforceaspect.c
	gcc -o dvforceaspect dvforceaspect.c

dvsplit: dvsplit.c
	gcc -o dvsplit dvsplit.c

dvrepfill: dvrepfill.c
	gcc -o dvrepfill dvrepfill.c

dvaudiorip: dvaudiorip.c
	gcc -o dvaudiorip dvaudiorip.c

dvaudiotail: dvaudiotail.c
	gcc -ldv -o dvaudiotail dvaudiotail.c

dv24pa-qt: dv24pa-qt.c
	gcc -o dv24pa-qt dv24pa-qt.c

dv2mpeg: dv2mpeg.c
	gcc -c -o dv2mpeg.o dv2mpeg.c
	gcc -ldv -lavcodec -o dv2mpeg dv2mpeg.o

clean:
	rm -f *.o dvmassagetime dvdumptimecodes dvsplit dvrepfill dvaudiorip dvaudiotail dvforceaspect dv2mpeg dv24pa-qt

install:
	cp -f dvmassagetime dvdumptimecodes dvsplit dvrepfill dvaudiorip dvaudiotail dvforceaspect dv2mpeg dv24pa-qt /usr/bin/

