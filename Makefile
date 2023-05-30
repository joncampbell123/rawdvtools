all: dvdumptimecodes dvsplit dvaudiorip dvforceaspect dv24pa-qt

dvdumptimecodes: dvdumptimecodes.c
	gcc -o dvdumptimecodes dvdumptimecodes.c

dvforceaspect: dvforceaspect.c
	gcc -o dvforceaspect dvforceaspect.c

dvsplit: dvsplit.c
	gcc -o dvsplit dvsplit.c

dvaudiorip: dvaudiorip.c
	gcc -o dvaudiorip dvaudiorip.c

dv24pa-qt: dv24pa-qt.c
	gcc -o dv24pa-qt dv24pa-qt.c

clean:
	rm -f *.o dvdumptimecodes dvsplit dvaudiorip dvforceaspect dv24pa-qt

install:
	cp -f dvdumptimecodes dvsplit dvaudiorip dvforceaspect dv24pa-qt /usr/bin/

