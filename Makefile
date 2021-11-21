DFLAGS		:= -g
GFLAGS		:= -I ../base/utils -I ../base/include -Wall -I /usr/include/mysql

GPSMASTER_SRC	:= MainGpsMaster.cxx
GPSMASTER_OBJ	:= $(GPSMASTER_SRC:.cxx=.o)

all:	GpsMaster

%.o:	%.cxx
	g++ $(DFLAGS) $(GFLAGS) -o $@ -c $<

%.o:	%.c
	gcc $(DFLAGS) $(GFLAGS) -o $@ -c $<

GpsMaster: $(GPSMASTER_OBJ)
	g++ $(DFLAGS) $(GFLAGS) -o $@ $(GPSMASTER_OBJ) ../base/utils/libutils.a -levent -lmysqlclient -lpthread

clean:
	rm -f $(GPSMASTER_OBJ)
	rm -f GpsMaster

