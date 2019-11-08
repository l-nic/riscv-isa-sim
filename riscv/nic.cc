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

nic_t::nic_t() {
	cerr << "Starting lnic" << endl;
	if (nic_config_data == nullptr) {
		return;
	}
	cerr << "Non-null config data" << endl;
	string config_data(nic_config_data);
	vector<string> split_config_data = split(config_data, ",");
	if (split_config_data.size() < 2) {
		cerr << "Own ID must be defined" << endl;
		exit(-1);
	}
	cerr << "Processed config data" << endl;
	own_id = stol(split_config_data[0]);
	for (size_t i = 1; i < split_config_data.size(); i++) {
		server_ids.push_back(stol(split_config_data[i]));
	}
	cerr << "Getting id map" << endl;
	id_addr_map = get_id_addr_map();
	message_ids[MAP_ID] = sizeof(MapMessage) / 8;
	message_ids[REDUCE_ID] = sizeof(ReduceMessage) / 8;
	cerr << "Id map populated" << endl;

	server_socket = start_server_socket(id_addr_map[own_id].port);
	if (server_socket < 0) {
		cerr << "Failed to start server socket" << endl;
		exit(-1);
	}
	all_threads.push_back(move(thread([this] () {
		listen_for_connections();
	})));
	cerr << "Listen thread launched" << endl;

	this_thread::sleep_for(chrono::milliseconds(LISTEN_CONNECT_DELAY_MS));
	for (uint64_t server_id : server_ids) {
		// Connect to each listed server id
		int sockfd = start_client_socket(id_addr_map[server_id].ip_address, id_addr_map[server_id].port);
		if (sockfd < 0) {
			cerr << "Error connecting to server at ip address " << id_addr_map[server_id].ip_address << " and port " << id_addr_map[server_id].port << endl;
			continue;
		}
		id_addr_map[server_id].connectionfd = sockfd;
	}
}

nic_t::~nic_t() {
	for (auto& t : all_threads) {
		t.join();
	}
}

void nic_t::store_uint64(reg_t data) {
	if (nic_config_data == nullptr) {
		return;
	}
	uint64_t word = data;
	if (write_message_index == 0) {
		// Start of a new message, set its type
		uint64_t message_type_id = word;
		uint64_t message_size = message_ids[message_type_id];
		current_write_message = reinterpret_cast<Message*>(new uint64_t[message_size]);
		current_write_message->message_type_id = message_type_id;
		write_message_index++;
	} else {
		// Other data in the message, fill it in
		reinterpret_cast<uint64_t*>(current_write_message)[write_message_index] = word;
		write_message_index++;
	}

	if (write_message_index == message_ids[current_write_message->message_type_id]) {
		// Message is complete, write it out to the queue
		write_queue.push(current_write_message); // No locks on this since it's only used in one thread in run_task
		write_message_index = 0;
		flush_write_queue();
	}
}

reg_t nic_t::load_uint64() {
	if (nic_config_data == nullptr) {
		return 0;
	}
	cerr << "Lnic reading data" << endl;
	if (current_message && message_index < message_ids[current_message->message_type_id]) {
		// We have a current message and we're not done with it yet
		uint64_t next_word = reinterpret_cast<uint64_t*>(current_message)[message_index];
		message_index++;
		return next_word;
	}

	// We need to read a new message
	read_lock.lock();
	if (read_queue.empty()) {
		cerr << "Tried to read from an empty lnic queue!" << endl;
		exit(-1);
	}
	if (current_message) {
		delete [] current_message; // TODO: Check that this is the right kind of delete.
		// (It was created as an array, so probably.)
	}
	message_index = 0;
	current_message = read_queue.front();
	read_queue.pop();
	uint64_t next_word = reinterpret_cast<uint64_t*>(current_message)[message_index];
	message_index++;
	read_lock.unlock();
	return next_word;
}

void nic_t::set_enable(reg_t data) {
	if (data != 0) {
		enable = true;
	}
}

bool nic_t::get_enable() {
	return enable;
}

// Mimics a read of lnic read queue ready status register
bool nic_t::lnic_ready() {
	if (nic_config_data == nullptr) {
		return false;
	}
	read_lock.lock();
	bool ready = !read_queue.empty();
	read_lock.unlock();
	// Shouldn't be a problem to read this without a lock, since the queue is only
	// being drained by one thread, so there's no risk of getting a true here and
	// then having the queue actually be empty.
	return ready;
}

uint64_t nic_t::get_child_id(uint64_t own_id, uint64_t i) {
	return (BRANCHING_FACTOR * own_id) - (BRANCHING_FACTOR - 2) + i;
}

void nic_t::flush_write_queue() {
	// This function will for now require that the first non-message-id field of every message specifies its
	// destination id. This will be easier to set up than supporting out-of-band metadata (i.e. tcp/ip
	// or udp setup info sent via separate control instructions).
	while (!write_queue.empty()) {
		Message* to_send = write_queue.front();
		write_queue.pop();
		cerr << "Writing message with type " << to_send->message_type_id << " and dest " << to_send->destination_id << endl;
		int write_fd = id_addr_map[to_send->destination_id].connectionfd;
		ssize_t written_len = write(write_fd, to_send, message_ids[to_send->message_type_id]*8);
		if (written_len <= 0) {
			cerr << "Error writing to destination " << to_send->destination_id << endl;
		}
		delete [] to_send;
	}
}

void nic_t::listen_for_connections() {
	while (true) {
		struct sockaddr_in addr;
		socklen_t addr_len = sizeof(addr);
		cout << "Waiting for connection..." << endl;
		int connectionfd = accept(server_socket, (struct sockaddr *)
				&addr, &addr_len);
		if (connectionfd < 0) {
			cerr << "Server connection accept attempt failed."
				<< endl;
			break;
		}
		if (addr.sin_family != AF_INET) {
			cerr << "Server accepted non-internet connection."
				<< endl;
			break;
		}
		uint16_t port = ntohs(addr.sin_port);
		char dst_cstring[INET_ADDRSTRLEN + 1];
		memset(dst_cstring, 0, INET_ADDRSTRLEN + 1);
		inet_ntop(AF_INET, &addr.sin_addr.s_addr, dst_cstring,
				INET_ADDRSTRLEN + 1);
		string ip_address(dst_cstring);
		nic_t::connection_t conn;
		conn.connectionfd = connectionfd;
		conn.port = port;
		conn.ip_address = ip_address;
		all_threads.push_back(move(thread([this, conn] () {
						handle_connection(conn); })));
	}
}

int nic_t::start_server_socket(uint16_t port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
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

	int listen_retval = listen(sockfd, CONN_BACKLOG);
	if (listen_retval < 0) {
		cerr << "Server socket listen failure." << endl;
		return -1;
	}
	return sockfd;
}

void nic_t::handle_connection(nic_t::connection_t conn) {
	int message_index = 0;
	while (true) {
		// This scheme requires sending the message type id first
		uint64_t message_type_id;
		cout << "Connection waiting for data..." << endl;
		ssize_t actual_len = read(conn.connectionfd, &message_type_id, 8);
		if (actual_len <= 0) {
			cerr << "Read failure at server in header" << endl;
			break;
		}
		cerr << "Message type id is " << message_type_id << endl;
		uint64_t message_size = message_ids[message_type_id];
		uint64_t* buffer = new uint64_t[message_size];
		buffer[0] = message_type_id;
		cerr << "Message size is " << message_size << endl;
		actual_len = read(conn.connectionfd, buffer + 1, (message_size-1)*8);
		if (actual_len <= 0) {
			cerr << "Read failure at server." << endl;
			break;
		}
		read_lock.lock();
		read_queue.push(reinterpret_cast<Message*>(buffer));
		read_lock.unlock();
	
	}
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

map<uint64_t, nic_t::connection_t> nic_t::get_id_addr_map() {
	ifstream id_addr_file;
	id_addr_file.open(ID_ADDR_FILENAME);
	string line;
	bool first_line = true;
	map<uint64_t, nic_t::connection_t> id_addr_map;
	while (getline(id_addr_file, line)) {
		if (line.empty()) {
			continue;
		}
		if (first_line) {
			first_line = false;
			continue;
		}
		vector<string> split_line = split(line, ",");
		uint64_t id = stol(split_line[0]);
		string ip_addr = split_line[1];
		uint16_t port = stoi(split_line[2]);
		id_addr_map[id] = {-1, port, ip_addr};
	}
	id_addr_file.close();
	return id_addr_map;
}

int nic_t::start_client_socket(string ip_address, uint16_t port) {
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		return -1;
	}
	struct in_addr network_order_address;
	int ip_conversion_retval = inet_aton(ip_address.c_str(), &network_order_address);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = network_order_address;
	int connect_retval = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
	if (connect_retval < 0) {
		close(sockfd);
		return -1;
	}
	return sockfd;
}

