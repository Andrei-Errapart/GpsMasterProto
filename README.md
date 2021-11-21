GPS DATA FORWARDER
------------------

Listens on two ports, "Bees" and "Honey". Clients are divided into
two groups based on which ports they have connected to.

Everything read from group "Honey" is forwarded to the clients
on the group "Bees". If "Twoway" is set, the data is sent the other
direction, too.

INITIALIZATION FILE
-------------------

Section GpsMasterProto:
PortBees	-- Port number for bees (rovers). Default: 2003.
PortHoney	-- Port number for honey (base station). Invalid if ServerHoney set. Default: 2004.
ServerHoney	-- Server IP and port for honey (IP base station). Invalidates PortHoney. Default: none.
Twoway		-- Send data from "Bees" to "Honey" iff set to "true".
RtcmInjectType	-- If present, inject custom RTCM packets with given type code and random contents
		   into the output stream (Bees). Period is once a second when RTCM stream is present,
		   when data traffic is absent, only once per 5 seconds.
		   NB! For this to work, input stream MUST BE in RTCM format.
RtcmInject23=period;AntennaDescriptor;AntennaSerial;SetupID
		-- Inject RTCM 23 messages into output stream.
			period - injection period, seconds.
			AntennaDescriptor - Antenna descriptor, up to 32 characters.
			AntennaSerial - Antenna serial, up to 32 characters.
			SetupID	- Setup identifier, number in range 0..255.
RtcmInject24=period;EcefX;EcefY;EcefZ;AntennaHeight
		-- Inject RTCM 23 messages into output stream.
			period - injection period, seconds.
			EcefX;EcefY;EcefZ - X,Y,Z coordinates in Earth-Center-Earth-Fixed
				coordinates, in range +-13,743,895.3472 m.
			AntennaHeight - Antenna height, meters, in range 0 .. 26.2144 m
AllowAll	-- shall we allow all to access? default is false.

Configuration example:
----------------------------------------
[GpsMasterProto]
PortBees=2003
PortHoney=2004
Twoway=true
----------------------------------------


REFERENCES
----------
RTCM Message 23 - pages 106-109 in "c10402.3 - RTCM VER 2-323.pdf"
RTCM Message 24 - pages 110-112 in "c10402.3 - RTCM VER 2-323.pdf"

