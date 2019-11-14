#include <unistd.h>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <iostream>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <map>
#include <fstream>
#include <queue>

#include "nic.h"

extern const char * nic_config_data;

using namespace std;

void nic_t::populate_addr_maps() {
	ifstream id_addr_file;
	id_addr_file.open(ID_ADDR_FILENAME);
	string line;
	bool first_line = true;
	while (getline(id_addr_file, line)) {
		if (line.empty()) {
			continue;
		}
		if (first_line) {
			first_line = false;
			continue;
		}
		vector<string> split_line = split(line, ",");
		string fake_addr = split_line[0];
		string real_addr = split_line[1];
		uint16_t real_port = stoi(split_line[2]);
		_real_addr_map[fake_addr] = make_pair(real_addr, real_port);
		_fake_addr_map[make_pair(real_addr, real_port)] = fake_addr;
	}
	id_addr_file.close();
}

void nic_t::fake_to_real_addr(string& real_addr, uint16_t& real_port, string& fake_addr) {
	real_addr = _real_addr_map[fake_addr].first;
	real_port = _real_addr_map[fake_addr].second;
}

void nic_t::real_to_fake_addr(string& real_addr, uint16_t& real_port, string& fake_addr) {
	fake_addr = _fake_addr_map[make_pair(real_addr, real_port)];
}

uint64_t nic_t::get_port_id() {
	return 1; // TODO: Will eventually do a port id lookup based on the thread
}

nic_t::nic_t() {
	if (nic_config_data == nullptr) {
		return;
	}
	_own_ip_address(nic_config_data);
	populate_addr_maps();
	string real_addr;
	uint16_t real_port;
	fake_to_real_addr(real_addr, real_port, _own_ip_address);
	_server_socket = start_server_socket(real_port);
	if (_server_socket < 0) {
		cerr << "Failed to start server socket" << endl;
		exit(-1);
	}
	_write_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (_write_socket < 0) {
		printf("Failed to start write socket\n");
		exit(-1);
	}
	_all_threads.push_back(move(thread([this] () {
		listen_for_datagrams();
	})));
}

nic_t::~nic_t() {
	for (auto& t : _all_threads) {
		t.join();
	}
}

void nic_t::store_uint64(reg_t data) {
	if (nic_config_data == nullptr) {
		return;
	}
	if (!_enable) {
		return;
	}
	// Technically, this allows starting to send a message before
	// defining the per-message data. The actual write-out will fail though.
	uint64_t port_id = get_port_id();
	_write_message[port_id].words.push_back(data);
}

reg_t nic_t::load_uint64() {
	if (nic_config_data == nullptr) {
		return 0;
	}
	if (!_enable) {
		return 0;
	}
	// Read queue will now be a map of queues of nic_word_t's, wher
	// each nic_word_t contains a uint64_t and a boolean to indicate whether
	// it's the last word in the current message.
	// The number of messages in the queue will be determined by the
	// number of message end markers, meaning that a partially arrived
	// message will register as 0, but a partially consumed message will register
	// as 1.
	uint64_t port_id = get_port_id(); // Eventually based off the thread, always 1 for now
	_read_lock[port_id].lock();
	if (_read_queue[port_id].empty()) {
		printf("Tried to read from empty queue %ld\n", port_id);
		exit(-1);
	}
	nic_t::read_word_t next_word = _read_queue[port_id].front();
	_read_queue[port_id].pop();
	if (next_word.is_last_word) {
		_is_last_word[port_id] = true;
		_num_read_messages[port_id]--;
	} else {
		_is_last_word[port_id] = false; // This means that if we keep reading
		// past the end of one message the lrdend register will reset.
	}
	_read_lock[port_id].unlock();


	// Update the per message metadata if necessary. (I.e. the new one is non-null.)
	if (next_word.per_message_data) {
		if (_per_message_data[port_id] && _per_per_message_data[port_id] != next_word.per_message_data) {
			delete _per_message_data[port_id];
		}
		_per_message_data[port_id] = next_word.per_message_data;
	}
	return next_word;
}

uint64_t nic_t::is_last_word_read() {
	return _is_last_word[get_port_id()];
}

uint64_t nic_t::read_message_src_ip_lower() {
	string ip_addr = _per_message_data[get_port_id()].src_ip_addr;
	vector<string> split_addr = split(ip_addr, ".");
	uint64_t result = 0;
	result |= stoi(split_addr[0]) << (8*3);
	result |= stoi(split_addr[1]) << (8*2);
	result |= stoi(split_addr[2]) << 8;
	result |= stoi(split_addr[3]);
	return result;
}

uint64_t nic_t::read_message_src_ip_upper() {
	return 0; // Not yet supported
}

uint64_t nic_t::read_message_src_port() {
	return _per_message_data[get_port_id()].src_port_id;
}

void nic_t::set_enable(reg_t data) {
	if (data != 0) {
		_enable = true;
	} else {
		_enable = false;
	}
}

bool nic_t::get_enable() {
	return _enable;
}

// Mimics a read of lnic read queue ready status register
uint64_t nic_t::num_messages_ready() {
	if (nic_config_data == nullptr) {
		return 0;
	}
	if (_enable) {
		return 0;
	}
	uint64_t port_id = get_port_id();
	_read_lock[port_id].lock();
	uint64_t num_read_messages = _num_read_messages[port_id];
	_read_lock[port_id].unlock();

	// Shouldn't be a problem to return this without a lock, since the queue is only
	// being drained by one thread, so there's no risk of getting a true here and
	// then having the queue actually be empty.
	return num_read_messages;
}

void nic_t::write_message_end() {
	if (!_enable) {
		return;
	}
	uint64_t port_id = get_port_id();
	if (!_write_message[port_id].per_message_data) {
		return; // We need the per message data or we'll have no idea where to send this message
	}

	// Build our payload for sending
	uint64_t datagram_len = (_write_message[port_id].words.size() / sizeof(uint64_t)) + sizeof(nic_t::header_data_t);
	uint8_t* datagram = new uint8_t[datagram_len];
	nic_t::header_data_t* header_data = (nic_t::header_data_t*)datagram;
	header_data->magic = LNIC_MAGIC;
	header_data->src_port_id = _write_message[port_id].per_message_data.src_port_id;
	header_data->dst_port_id = _write_message[port_id].per_message_data.dst_port_id;
	uint64_t* data_start = (uint64_t*)(datagram + sizeof(nic_t::header_data_t));
	for (size_t i = 0; i < _write_message[port_id].words.size(); i++) {
		data_start[i] = _write_message[port_id].words[i];
	}
	_write_message[port_id].words.clear();

	// Actually route the packet in the simulated system
	string fake_ip_address = _write_message[port_id].per_message_data.dst_ip_addr;
	string real_ip_addr;
	uint16_t real_udp_port;
	fake_to_real_addr(real_ip_addr, real_udp_port, fake_ip_address);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(real_udp_port);
	struct in_addr network_order_address;
    int ip_conversion_retval = inet_aton(real_ip_addr.c_str(), &network_order_address);
    if (ip_conversion_retval == 0) {
        printf("Ip conversion failure\n");
        exit(-1);
    }
    addr.sin_addr = network_order_address;

    // Send the packet
	ssize_t bytes_written = sendto(_write_socket, datagram, datagram_len, 0, (const struct sockaddr *)&addr, sizeof(addr));
	delete datagram;
	if (bytes_written <= 0) {
		printf("Error writing packet\n");
		exit(-1);
	}
}

void set_own_port_id(uint64_t attempted_port_id) {
	if (!_enable) {
		return;
	}
	// Don't do anything yet, the port id can only be 1 for now
	_write_message[1].per_message_data.src_port_id = 1;
}

void set_dst_ip_lower(uint64_t lower_ip_bits) {
	if (!_enable) {
		return;
	}
	lower_ip_bits &= 0xFFFFFFFF;
	stringstream ss;
	uint8_t first = (lower_ip_bits >> (8*3)) & 0xFF;
	uint8_t second = (lower_ip_bits >> (8*2)) & 0xFF;
	uint8_t third = (lower_ip_bits >> 8) & 0xFF;
	uint8_t fourth = lower_ip_bits & 0xFF;
	ss << first << "." << second << "." << third << "." << fourth;
	_write_message[get_port_id()].per_message_data.dst_ip_addr = ss.str();
}

void set_dst_ip_upper(uint64_t upper_ip_bits) {
	// Only ipv4 for now
}

void set_dst_port_id(uint64_t dst_port_id) {
	if (!_enable) {
		return;
	}
	_write_message[get_port_id()].per_message_data.dst_port_id = dst_port_id;
}


void nic_t::handle_lnic_datagram(uint8_t* datagram, ssize_t datagram_len, string src_ip_address) {
	// We now have the raw data and the src ip address, same as we would
	// after deciphering the outer layers of a packet in a hardware implementation.
	// This implementation assumes that each packet contains either one full message
	// or part of one full message. Given that we're trying to minimize latency,
	// packing multiple messages into one packet (thus delaying the earlier ones)
	// would probably be a bad idea.

	// Everything in this section deals with lnic processing. This is fixed.
	if (datagram_len < sizeof(nic_t::header_data_t)) {
		return; // Drop packets that are too short
	}
	nic_t::header_data_t* lnic_header = (nic_t::header_data_t*)datagram;
	uint32_t lnic_magic = lnic_header->magic;
	if (lnic_magic != LNIC_MAGIC) {
		return; // Not an lnic packet, should never have been sent here
	}
	if (datagram_len - sizeof(nic_t::header_data_t) % sizeof(uint64_t) != 0) {
		// All lnic packets should have word-aligned data
		return;
	}
	uint64_t* data_ptr = (uint64_t*)(datagram + sizeof(nic_t::header_data_t));
	uint64_t word_len = (datagram_len - sizeof(nic_t::header_data_t)) / sizeof(uint64_t);
	uint64_t port_id = lnic_header->dst_port_id;
	nic_t::header_data_t* per_message_data = new nic_t::per_message_data;
	per_message_data->dst_port_id = lnic_header->dst_port_id;
	per_message_data->src_port_id = lnic_header->src_port_id;
	per_message_data->src_ip_addr = src_ip_addr;
	per_message_data->dst_ip_addr = _own_ip_address;

	// Everything in this section deals with pushing the received data into the
	// read queue for its destination thread. This section can be changed to
	// mimic implementing a transport protocol in hardware. Anything that gets
	// queued up still needs an end-marker and a 4-tuple, but there's no
	// requirement that that happen right away.
	_read_lock[port_id].lock();
	for (uint64_t i = 0; i < word_len; i++) {
		bool is_end = (i == word_len - 1);
		_read_queue[port_id].push({data_ptr[i], is_end, per_message_data});
	}
	_num_read_messages[port_id]++;
	_read_lock[port_id].unlock();
}

void nic_t::listen_for_datagrams() {
	uint8_t datagram[DATAGRAM_SIZE];
	while (true) {
		struct sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);
		ssize_t bytes_received = recvfrom(_server_socket, datagram, DATAGRAM_SIZE, MSG_WAITALL, (struct sockaddr *)&addr, &addr_len);

		if (bytes_received < 0) {
			cerr << "Datagram receive attempt failed."
				<< endl;
			break;
		}
		if (addr.sin_family != AF_INET) {
			cerr << "Server accepted non-internet datagram."
				<< endl;
			break;
		}
		uint16_t port = ntohs(addr.sin_port);
		char dst_cstring[INET_ADDRSTRLEN + 1];
		memset(dst_cstring, 0, INET_ADDRSTRLEN + 1);
		inet_ntop(AF_INET, &addr.sin_addr.s_addr, dst_cstring,
				INET_ADDRSTRLEN + 1);
		string ip_address(dst_cstring);
		string fake_ip_address;
		real_to_fake_addr(ip_address, port, fake_ip_address);
		handle_lnic_datagram(datagram, bytes_received, fake_ip_address);
	}
}

int nic_t::start_server_socket(uint16_t port) {
	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		cerr << "Server socket creation failed." << endl;
		return -1;
	}
	int sockopt_enable = 1;
	int setopt_retval = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
			&sockopt_enable, sizeof(int));
	if (setopt_retval < 0) {
		cerr << "Server socket option setting failed." << endl;
		return -1;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	int bind_retval = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
	if (bind_retval < 0) {
		cerr << "Server socket bind failure." << endl;
		return -1;
	}

	return sockfd;
}

vector<string> nic_t::split(string input, string delim) {
	vector<string> to_return;
	while (true) {
		size_t delim_pos = input.find(delim);
		if (delim_pos == string::npos) {
			break;
		}
		to_return.push_back(input.substr(0, delim_pos));
		input = input.substr(delim_pos + 1);
	}
	to_return.push_back(input);
	return to_return;
}
