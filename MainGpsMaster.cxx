/**
vim:	ts=4
vim:	shiftwidth=4
*/

/// \file GpsMaster main file.
///
/// See \c gps_server.doc for complete specification.

#ifdef WIN32
#pragma warning(disable:4786) 
#include <winsock2.h>	// struct timeval.
#endif

#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <vector>	// std::vector
#include <string>	// std::string
#include <list>		// std::list

#include <stdio.h>	// printf, sprintf
#include <stdarg.h>	// varargs.

#include <event.h>	// libevent.

#include <utils/mysockets.h>	// open_server_socket
#include <utils/util.h>		// Error.
#include <utils/Config.h>	// FileConfig
#include <utils/myrtcm.h>

using namespace utils;

/// Shortcut.
typedef std::vector<unsigned char>	ArrayOfChars;

typedef enum {
	GROUP_BEES,	///< One group.
	GROUP_HONEY	///< Second group.
} GROUP;

/// Configuration.
namespace config {
	/// Bees looking for honey.
	static unsigned int
	PORT_BEES = 2003;

	/// Honey waiting for bees.
	static unsigned int
	PORT_HONEY = 2004;

	/// Honeyserver Host:Port, if any.
	static std::string	honeyserver;

	/// Is it two-way connection?
	static bool		twoway = false;	///< Is it two-way system?

	/// Type of random RTCM packets injected...
	Option<unsigned int>	RtcmInjectType;

	/// Shall we allow all to access.
	static bool				AllowAll = false;
	
	/// Maximum number of idle ticks for given client...?
	static const unsigned int	MAX_IDLE_TICKS = 10;
	/// RTCM injection period.
	static const double		RTCM_INJECT_PERIOD		= 5.0;
	/// RTCM injection period when inactive.
	static const double		RTCM_INJECT_INACTIVE_PERIOD	= 15.0;
}

/*****************************************************************************/
static void
generate_bogus_rtcm(
	RtcmEncoder&		encoder,
	ArrayOfChars&		streambuffer)
{
	std::vector<isgps30bits_t>	bits(RTCM_WORDS_MAX);
	struct rtcm_msg_t*			msg = reinterpret_cast<struct rtcm_msg_t*>(&bits[0]);

	// Randomize contents.
	{
		unsigned char*		ptr = reinterpret_cast<unsigned char*>(&bits[0]);
		const unsigned int	nchars = bits.size()*4;

		for (unsigned int i=0; i<nchars; ++i) {
			const int	rnd = rand();
			const unsigned char	crand = rnd ^ (rnd >> 8);
			ptr[i] = crand;
		}
	}

	// W1
	// msg->w1._pad		= 0;
	msg->w1.preamble	= RTCM_PREAMBLE_PATTERN;
	msg->w1.msgtype	= config::RtcmInjectType();
	msg->w1.refstaid	= 33;

	// W2
	// msg->w2._pad		= 0;
	// msg->w2.zcnt		= 0;
	// msg->w2.sqnum	= 0;
	msg->w2.frmlen	= bits.size()-2;

	const unsigned int	offset = streambuffer.size();
	encoder.Encode(streambuffer, msg);
#if (0)
	const unsigned int	nchars = streambuffer.size() - offset;
	// check it...
	{
		bool	passed = false;
		struct gps_packet_t	lexer = { 0 };

		isgps_init(&lexer);

		for (unsigned int i=0; i<nchars; ++i) {
			const enum isgpsstat_t	res = rtcm_decode(&lexer, streambuffer[offset+i]);
			if (res == ISGPS_MESSAGE) {
				passed = true;
			}
		}

		printf("generate_bogus_rtcm: %s\n", passed ? "OK" : "FAILED");
	}
#endif
}

class GpsMaster;

/// Client info.
class TCP_CLIENT {
public:
	int			fd;		///< Client socket. Default: -1.
	std::string		connection_name;///< Hostname and source port, if any.
	struct bufferevent*	event;		///< Buffered events. Default: 0.
	GpsMaster*		server;		///< Points back to server. Default: 0.
	GROUP			group;		///< Bee or honey. Default: GROUP_BEES.
	unsigned int		idle_ticks;	///< Number of idle ticks. Default: 0.
	unsigned int		bytes_read;	///< Bytes read from this client. Default: 0.
	unsigned int		bytes_written;	///< Bytes written to this client. Default: 0.

	/// Construct with default values.
	TCP_CLIENT()
	: fd(INVALID_SOCKET),
	  event(0),
	  server(0),
	  group(GROUP_BEES),
	  idle_ticks(0),
	  bytes_read(0),
	  bytes_written(0)
	{
	}
}; // struct TCP_CLIENT.

/*****************************************************************************/
/*****************************************************************************/
/// Server main.
class GpsMaster {
private:
	struct event_base*	evb_;			///< Event system.
	int			bees_lsocket_;		///< Connection socket for bees.
	int			honey_lsocket_;		///< Connection socket for honey.
	int			honeyserver_socket_;	///< Connection socket for honey server.
	struct event		bees_lsocket_event_;	///< TCP port "listen" events.
	struct event		honey_lsocket_event_;	///< TCP port "li
	struct timeval		timer_period_;		///< Timer period, every 5 minutes.
	struct event		timer_event_;		///< Timer event.

	std::list<TCP_CLIENT*>	tcp_clients_;		///< List of clients :)
	ArrayOfChars		tcp_read_buffer_;	///< Temporary buffer for TCP reads.

	ArrayOfChars		honeyserver_read_buffer_;	///< Temporary buffer for TCP reads.
	unsigned int		honeyserver_bytes_read_;
	unsigned int		honeyserver_bytes_read_last_;
	struct bufferevent*	honeyserver_event_;		///< Buffered events. Default: 0.

	/** RTCM injection . */
	RtcmEncoder			rtcm_encoder_;			///< RTCM encoder.
	RtcmDecoder			rtcm_decoder_;			///< RTCM decoder.
	my_time				rtcm_inject_time_;		///< Last time RTCM was injected.
	my_time				rtcm_inactivity_time_;	///< Last time
	struct timeval		rtcm_timer_period_;		///< Timer period, every second.
	struct event		rtcm_timer_event_;		///< Timer event.
public:
	/*****************************************************************************/
	/// Log a message preceded by time in format "HHMMSS".
	static void
	log(			const char*		fmt, ...)
	{
		my_time	rtime;
		get_my_time(rtime);
		printf("%02d%02d%02d: ", rtime.hour, rtime.minute, rtime.second);

		va_list	ap;
		va_start(ap, fmt);
		vfprintf(stdout, fmt, ap);
		va_end(ap);
		printf("\n");
		fflush(stdout);
	}

private:
	/*****************************************************************************/
	/// Send data to given client.
	void
	send_data(	TCP_CLIENT*		tcp_client,
			const ArrayOfChars&	data)
	{
		if (config::twoway || tcp_client->group!=GROUP_HONEY && data.size()>0) {
			bufferevent_write(tcp_client->event, const_cast<void*>((void*)(&data[0])), data.size());
			tcp_client->bytes_written += data.size();
		}
	}

	/*****************************************************************************/
	/**
	Send event to the list of clients filtered by \c target_group

	If RtcmInjectType is defined, the data has to be valid RTCM data.
	 */
	void
	send_data_except_me(	const std::list<TCP_CLIENT*>&	tcp_clients,
				const GROUP			target_group,
				TCP_CLIENT*			me,
				const ArrayOfChars&		data)
	{
		ArrayOfChars	infected_data;
		bool			use_infected_data = config::RtcmInjectType.some() && target_group==GROUP_BEES;

		// Shall we inject RTCM data if possible?
		if (use_infected_data) {
			const my_time	time_now = get_my_time();
			const double	dt = time_now - rtcm_inject_time_;

			// First, copy over original data.
			// Parse the data and find first position to inject..
			for (unsigned int i=0; i<data.size(); ++i) {
				const struct rtcm_msg_t*	msg = rtcm_decoder_.Feed(data[i]);
				if (msg != 0) {
					log("Got RTCM packet %d, length %d words", msg->w1.msgtype, msg->w2.frmlen);
					rtcm_encoder_.Encode(infected_data, msg);
				}
			}

			// Shalle 
			if (dt > config::RTCM_INJECT_PERIOD) {
				log("RTCM bogus packet, type %d, injected.", config::RtcmInjectType());
				// yes, inject if possible. :P
				generate_bogus_rtcm(rtcm_encoder_, infected_data);

				rtcm_inject_time_ = time_now;
			}
		}

		for (std::list<TCP_CLIENT*>::const_iterator it=tcp_clients.begin(); it!=tcp_clients.end(); ++it) {
			TCP_CLIENT*	tcp_client = *it;
			if (tcp_client!=me && tcp_client->group == target_group) {
				send_data(tcp_client, use_infected_data ? infected_data : data);
			}
		}

		/** record last time of activity. */
		rtcm_inactivity_time_ = get_my_time();
	}

	/***************************************************************/
	/// Shutdown and delete TCP client.
	void
	kill_tcp_client(	TCP_CLIENT*		tcp_client,
				const char*		reason)
	{
		log("Shutting down client %s. Reason: %s", tcp_client->connection_name.c_str(), reason);
		// Shutdown events.
		bufferevent_disable(tcp_client->event, EV_READ|EV_WRITE);
		bufferevent_free(tcp_client->event);
		tcp_client->event = 0;

		// Close socket.
		close_socket(tcp_client->fd);
		tcp_client->fd = INVALID_SOCKET;

		// Remove client from the chain...
		tcp_clients_.remove(tcp_client);

		delete tcp_client;
	}

	/*****************************************************************************/
	/// Timer passed...
	static void
	timer_handler(	int			fd,
			short			event,
			void*			_self)
	{
		GpsMaster*		server = reinterpret_cast<GpsMaster*>(_self);

		// 1. Report clients...
		{
			log("1-minute timer: .");
			for (std::list<TCP_CLIENT*>::const_iterator it=server->tcp_clients_.begin(); it!=server->tcp_clients_.end(); ++it) {
				TCP_CLIENT*	tcp_client = *it;
				switch (tcp_client->group) {
				case GROUP_HONEY:
					log("Honey %s, %d bytes read, %d bytes written.",
						tcp_client->connection_name.c_str(),
						tcp_client->bytes_read, tcp_client->bytes_written);
					break;
				case GROUP_BEES:
					log("Bee %s, %d bytes read, %d bytes written.",
						tcp_client->connection_name.c_str(),
						tcp_client->bytes_read, tcp_client->bytes_written);
					break;
				}
			}
		}

		// 2. Report connection.
		if (config::honeyserver.size()>0) {
			const bool	is_connected = server->honeyserver_socket_ != (int)INVALID_SOCKET;
			const int	delta_bytes = server->honeyserver_bytes_read_ - server->honeyserver_bytes_read_last_;
			log("Honeyserver: %s, %d bytes last minute, %d bytes total.",
					is_connected ? "connected" : "disconnected",
					delta_bytes,
					server->honeyserver_bytes_read_
					);

			server->honeyserver_bytes_read_last_ = server->honeyserver_bytes_read_;
			if (is_connected) {
				if (delta_bytes == 0) {
					server->honeyserver_reconnect("No data.");
				}
			} else {
				server->honeyserver_reconnect("Disconnection.");
			}
		}

		evtimer_add(&server->timer_event_, &server->timer_period_);
	}

	/*****************************************************************************/
	/// Timer passed...
	static void
	rtcm_timer_handler(	int		fd,
						short	event,
						void*	_self)
	{
		GpsMaster*		server = reinterpret_cast<GpsMaster*>(_self);
		const my_time	time_now = get_my_time();
		const double	dt = time_now - server->rtcm_inactivity_time_;

		// Inject bogus data???
		if (dt > config::RTCM_INJECT_INACTIVE_PERIOD) {
			log("RTCM bogus packet, type %d, injected.", config::RtcmInjectType());

			ArrayOfChars	infected_data;
			server->rtcm_inject_time_ = time_now;
			server->rtcm_inactivity_time_ = time_now;

			generate_bogus_rtcm(server->rtcm_encoder_, infected_data);

			const std::list<TCP_CLIENT*>&   tcp_clients = server->tcp_clients_;

			for (std::list<TCP_CLIENT*>::const_iterator it=tcp_clients.begin(); it!=tcp_clients.end(); ++it) {
				TCP_CLIENT*	tcp_client = *it;
				if (tcp_client->group == GROUP_BEES) {
					server->send_data(tcp_client, infected_data);
				}
			}
		}

		evtimer_add(&server->rtcm_timer_event_, &server->rtcm_timer_period_);
	}

	/***************************************************************/
	static void
	tcp_client_read_handler(struct bufferevent*	event,
				void*			_tcp_client)
	{
		TCP_CLIENT*	tcp_client = reinterpret_cast<TCP_CLIENT*>(_tcp_client);
		GpsMaster*	server = tcp_client->server;
		ArrayOfChars&	buffer = server->tcp_read_buffer_;

		// Read from buffer.
		unsigned int	so_far = 0;
		for (;;) {
			if (buffer.size() == so_far)
				buffer.resize(so_far<=0 ? 1024 : 2*so_far);
			int	this_round = bufferevent_read(event, &buffer[so_far], buffer.size() - so_far);
			if (this_round <= 0)
				break;
			so_far += this_round;
		}
		buffer.resize(so_far);
		tcp_client->bytes_read += so_far;

		// Detect EOF.
		if (so_far == 0) {
			server->kill_tcp_client(tcp_client, "TCP End-of-stream.");
			return;
		}

		// Forward good stuff.
		server->send_data_except_me(server->tcp_clients_, tcp_client->group==GROUP_BEES ? GROUP_HONEY : GROUP_BEES, tcp_client, buffer);
	}

	/***************************************************************/
	static void
	tcp_client_write_handler(struct bufferevent*	event,
				void*			_tcp_client)
	{
		TCP_CLIENT*	tcp_client = reinterpret_cast<TCP_CLIENT*>(_tcp_client);
		GpsMaster*	server = tcp_client->server;
		// printf("TCP client write event.\n");

		// Pass.

		(void)server;
	}

	/***************************************************************/
	static void
	tcp_client_error_handler(struct bufferevent*	event,
				short			what,
				void*			_tcp_client)
	{
		TCP_CLIENT*	tcp_client = reinterpret_cast<TCP_CLIENT*>(_tcp_client);
		GpsMaster*	server = tcp_client->server;

		server->kill_tcp_client(tcp_client, "TCP read error.");
	}

	/***************************************************************/
	/// String representation of \c addr.
	static std::string
	connection_name_of_sockaddr(	const struct sockaddr_in&	addr,
					std::string&			hostname)
	{
		std::string	r;
		
		// Detect hostname, if possible.
		hostname.resize(0);
		const struct hostent*	he = gethostbyaddr(&addr.sin_addr, sizeof(addr.sin_addr), AF_INET);
		if (he == 0) {
			const char*	ipname = inet_ntoa(addr.sin_addr);
			if (ipname == 0) {
				hostname = "Undetected";
			} else {
				hostname = ipname;
			}
		} else {
			hostname = he->h_name;
		}

		// Form return string.
		char		xbuf[1024];
		r += hostname;
		r += " ";
		sprintf(xbuf, "%d", addr.sin_port);
		r += xbuf;

		// yeah.
		return r;
	}

	/*****************************************************************************/
	/// Is client acceptable?
	static const bool
	is_client_ok(			const std::string&		hostname)
	{
		if (config::AllowAll) {
			return true;
		}
		// check internal connections from 194.204.26.104 
		if (hostname == "rockefeller") {
			return true;
		}
		if (hostname == "migw2.gprs.emt.ee") {
			return true;
		}
		if (hostname == "ns.mivar.ee") {
			return true;
		}
		if (hostname == "gprs-inet.elisa.ee") {
			return true;
		}
		if (hostname == "84-50-157-17-dsl.est.estpak.ee") {
			return true;
		}
		if (hostname == "62.65.35.147") {
			return true;
		}
		// Mivar.
		if (hostname == "89.219.150.227") {
			return true;
		}
		// Kalle
		if (hostname == "80.241.209.3") {
			return true;
		}
		{
			// check for EMT roaming...
			const std::string	emt_trail("gprs.emt.ee");
			const unsigned int	emt_len = emt_trail.size();
			const unsigned int	hlen = hostname.size();
			if (hlen > emt_len && hostname.substr(hlen-emt_len, emt_len) == emt_trail) {
				return true;
			}
		}

		// check .ee domain and Bravocom.
		const unsigned int	size = hostname.size();
		if (size>=3 && hostname.substr(size-3, 3)==".ee") {
			if (hostname.find("bravo") != std::string::npos) {
				return true;
			}
			if (hostname.find("norby") != std::string::npos) {
				return true;
			}
		}

		// otherwise bummer.
		return false;
	}

	/*****************************************************************************/
	/// TCP client accepted.
	void
	do_accept(	const int		fd,
			const GROUP		group)
	{
		struct sockaddr_in	addr;
#ifdef WIN32
		int			addrlen = sizeof(addr);
#else
		socklen_t		addrlen = sizeof(addr);
#endif
		int			sd = accept(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrlen);
		if (sd == -1) {
			log("Error: Failed to get client.\n");
		} else {
			// Check hostname.
			std::string		hostname;
			const std::string	connection_name = connection_name_of_sockaddr(addr, hostname);
			if (is_client_ok(hostname)) {
				// Initialize structure.
				TCP_CLIENT*		tcp_client = new TCP_CLIENT;
				tcp_client->fd	= sd;
				tcp_client->event = bufferevent_new(sd,
						tcp_client_read_handler,
						tcp_client_write_handler,
						tcp_client_error_handler,
						tcp_client);
				tcp_client->server	= this;
				tcp_client->group	= group;
				tcp_client->connection_name = connection_name;

				log("New client 0x%0x, connection %s!", sd, tcp_client->connection_name.c_str());

				// Add to client list.
				tcp_clients_.push_front(tcp_client);

				// Enable events.
				bufferevent_enable(tcp_client->event, EV_READ|EV_WRITE);
			} else {
				log("Bad client: hostname:%s, connection:%s", hostname.c_str(), connection_name.c_str());
				close_socket(sd);
			}
		}
	}

	/*****************************************************************************/
	/// Accept a BEE.
	static void
	bees_lsocket_handler(	int		fd,
				short		event,
				void*		_self)
	{
		GpsMaster*	server = reinterpret_cast<GpsMaster*>(_self);
		server->do_accept(fd, GROUP_BEES);
	}

	/*****************************************************************************/
	/// Accept a BEE.
	static void
	honey_lsocket_handler(	int		fd,
				short		event,
				void*		_self)
	{
		GpsMaster*	server = reinterpret_cast<GpsMaster*>(_self);
		server->do_accept(fd, GROUP_HONEY);
	}

	/***************************************************************/
	static void
	honeyserver_read_handler(
		struct bufferevent*	event,
		void*			_gpsmaster)
	{
		GpsMaster*	server = reinterpret_cast<GpsMaster*>(_gpsmaster);
		ArrayOfChars&	buffer = server->honeyserver_read_buffer_;

		// Read from buffer.
		unsigned int	so_far = 0;
		for (;;) {
			if (buffer.size() == so_far) {
				buffer.resize(so_far<=0 ? 1024 : 2*so_far);
			}
			int	this_round = bufferevent_read(event, &buffer[so_far], buffer.size() - so_far);
			if (this_round <= 0)
				break;
			so_far += this_round;
		}
		buffer.resize(so_far);
		server->honeyserver_bytes_read_ += so_far;

		// Detect EOF.
		if (so_far == 0) {
			server->honeyserver_close("Connection closed.");
		} else {
			// Forward good stuff.
			server->send_data_except_me(
				server->tcp_clients_,
				GROUP_BEES,
				NULL,
				buffer);
		}
	}

	/***************************************************************/
	static void
	honeyserver_write_handler(
		struct bufferevent*	event,
		void*			_gpsmaster)
	{
		GpsMaster*	server = reinterpret_cast<GpsMaster*>(_gpsmaster);
		(void)server;
		// printf("TCP client write event.\n");
		// Pass.
	}

	/***************************************************************/
	static void
	honeyserver_error_handler(
		struct bufferevent*	event,
		short			what,
		void*			_gpsmaster)
	{
		GpsMaster*	server = reinterpret_cast<GpsMaster*>(_gpsmaster);
		server->honeyserver_close("Read error.");
	}

	/*****************************************************************************/
	void
	honeyserver_close(	const std::string&	reason)
	{
		log("honeyserver_close reason: %s", reason.c_str());
		if (honeyserver_socket_ != (int)INVALID_SOCKET) {
			// Free buffer.
			bufferevent_disable(honeyserver_event_, EV_READ|EV_WRITE);
			bufferevent_free(honeyserver_event_);
			honeyserver_event_ = 0;

			// Close socket.
			close_socket(honeyserver_socket_);
			honeyserver_socket_ = INVALID_SOCKET;
		}
	}

	/*****************************************************************************/
	void
	honeyserver_reconnect(	const std::string&	reason)
	{
		log("honeyserver_reconnect reason: %s", reason.c_str());
		if (honeyserver_socket_ != (int)INVALID_SOCKET) {
			honeyserver_close(reason);
		}
		try {
			honeyserver_socket_ = connect_server_socket(config::honeyserver);
			honeyserver_event_ = bufferevent_new(
					honeyserver_socket_,
					honeyserver_read_handler,
					honeyserver_write_handler,
					honeyserver_error_handler,
					this
					);

			log("connected to honeyserver at %s", config::honeyserver.c_str());
			honeyserver_bytes_read_ = 0;
			honeyserver_bytes_read_last_ = 0;

			// Enable events.
			bufferevent_enable(honeyserver_event_, EV_READ|EV_WRITE);
		} catch (const std::exception& e) {
			log("connect %s, error: %s", config::honeyserver.c_str(), e.what());
		}
	}
public:
	/*****************************************************************************/
	/// Constructor - initialize to zero or reasonable defaults all stuff.
	GpsMaster()
	:
		evb_(0)
		,bees_lsocket_(INVALID_SOCKET)
		,honey_lsocket_(INVALID_SOCKET)
		,honeyserver_socket_(INVALID_SOCKET)
		,honeyserver_bytes_read_(0)
		,honeyserver_bytes_read_last_(0)
		,honeyserver_event_(0)
	{
		// Configuration changs, if any.
		try {
			FileConfig	cfg("GpsMasterProto.ini");
			cfg.load();
			cfg.set_section("GpsMasterProto");
			cfg.get_uint("PortBees", config::PORT_BEES);
			cfg.get_uint("PortHoney", config::PORT_HONEY);
			cfg.get_string("ServerHoney", config::honeyserver);
			cfg.get_bool("Twoway", false, config::twoway);
			cfg.get_bool("AllowAll", false, config::AllowAll);
			{
				int	rtcm_type = -1;
				bool	ok = cfg.get_int("RtcmInjectType", rtcm_type);
				if (ok && rtcm_type>0) {
					config::RtcmInjectType = rtcm_type;
				}
			}
		} catch (const std::exception& e) {
			log("Error loading configuration file GpsMasterProto.ini: %s", e.what());
		}

		// Winsock bad.
		try {
			load_winsock();
		} catch (const std::exception& e) {
			log("Error loading WinSock (%s), aborting.", e.what());
			throw;
		}

		// RTCM injection stuff...
		get_my_time(rtcm_inject_time_);
		get_my_time(rtcm_inactivity_time_);
	}

	/*****************************************************************************/
	/// Destructor - release resources.
	~GpsMaster()
	{
		unload_winsock();
	}

	/*****************************************************************************/
	/// Initalize & run server.
	void
	run()
	{
		// Initalize \c libevent.
		evb_ = reinterpret_cast<struct event_base*>(event_init());

		// Timer setup (period 60 seconds).
		event_set(&timer_event_, 0, EV_TIMEOUT|EV_PERSIST, timer_handler, this);
		timer_period_.tv_sec = 60;
		timer_period_.tv_usec = 0;
		evtimer_add(&timer_event_, &timer_period_);

		// RTCM injection timer setup (period 1 second).
		if (config::RtcmInjectType.some()) {
			event_set(&rtcm_timer_event_, 0, EV_TIMEOUT|EV_PERSIST, rtcm_timer_handler, this);
			rtcm_timer_period_.tv_sec = 1;
			rtcm_timer_period_.tv_usec = 0;
			evtimer_add(&rtcm_timer_event_, &rtcm_timer_period_);
		}

		// Setup BEES port.
		bees_lsocket_ = open_server_socket(config::PORT_BEES);
		event_set(&bees_lsocket_event_, bees_lsocket_, EV_READ|EV_PERSIST, bees_lsocket_handler,  this);
		event_add(&bees_lsocket_event_, 0);

		// Setup HONEY PORT or honey SERVER
		if (config::honeyserver.size()>0) {
			honeyserver_reconnect("Boot.");
		} else {
			honey_lsocket_ = open_server_socket(config::PORT_HONEY);
			event_set(&honey_lsocket_event_, honey_lsocket_, EV_READ|EV_PERSIST, honey_lsocket_handler,  this);
			event_add(&honey_lsocket_event_, 0);
		}

		// Run stuff..
		log("Running on ports %d (bees) and %d (honey), %s", config::PORT_BEES, config::PORT_HONEY, config::twoway ? "two-way" : "one-way");
		log("Access allowed %s\n", config::AllowAll ? "for everybody" : "only for mobile clients");
		event_dispatch();
	}
};

/*****************************************************************************/
/// Set up listening port and start running...
int
main(	int	argc,
	char**	argv)
{
	TRACE_SETFILE("GpsMaster-log.txt");

	// Initialize random number generator.
	srand(time(0));

	try {
		GpsMaster	server;
		server.run();
	} catch (const std::exception& e) {
		GpsMaster::log("Exception: %s", e.what());
	}
	return 0;
}

