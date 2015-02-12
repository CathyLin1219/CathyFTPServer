#include "stdafx.h"

#include "ftp_session.h"

#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

using boost::asio::ip::tcp;

orianne::ftp_session::ftp_session(boost::asio::io_service& _service, boost::asio::ip::tcp::socket& socket_) 
	: io_service(_service), acceptor(0), working_directory("/"), socket(socket_)
{
}

void orianne::ftp_session::set_root_directory(boost::filesystem::path const& directory) {
	root_directory = directory;
}

orianne::ftp_result orianne::ftp_session::set_username(const std::string& username) {
	return orianne::ftp_result(331, "Please enter your password. without check"); 
}

orianne::ftp_result orianne::ftp_session::set_password(const std::string& username) {
	return orianne::ftp_result(230, "Login successful.");
}

static std::string endpoint_to_string(boost::asio::ip::address_v4::bytes_type address, unsigned short port) {
	std::stringstream stream;
	stream << "(";
	for(int i=0; i<4; i++) 
		stream << (int)address[i] << ",";
	stream << ((port >> 8) & 0xff) << "," << (port & 0xff) << ")";

	return stream.str();
}

orianne::ftp_result orianne::ftp_session::set_passive() {
	static unsigned short port = 10000;

	if(acceptor == 0)
		acceptor = new tcp::acceptor(io_service, tcp::endpoint(tcp::v4(), ++port));
	
	std::string tmp_message = "Entering passive mode ";
	tmp_message.append(endpoint_to_string(socket.local_endpoint().address().to_v4().to_bytes(), port));

	return orianne::ftp_result (227, tmp_message);
}

orianne::ftp_result orianne::ftp_session::get_size(const std::string& filename) {
	std::stringstream stream;
	stream << boost::filesystem::file_size(root_directory / working_directory / filename);

	return orianne::ftp_result(213, stream.str());
}

orianne::ftp_result orianne::ftp_session::change_working_directory(const std::string& new_directory) {
	working_directory.assign(new_directory, boost::filesystem::path::codecvt());
	return orianne::ftp_result(250, "OK");
}

orianne::ftp_result orianne::ftp_session::set_type(const struct orianne::ftp_transfer_type& type) {
	return orianne::ftp_result(200, "Switching to Binary mode.");
}

orianne::ftp_result orianne::ftp_session::get_working_directory() {
	return orianne::ftp_result(257, working_directory.string());
}

orianne::ftp_result orianne::ftp_session::get_system() {
	return orianne::ftp_result(215, "WINDOWS");
}

static std::string get_list(const boost::filesystem::path& path) {
	using namespace boost::filesystem;
	std::stringstream stream;

	for(directory_iterator it(path); it != directory_iterator(); it++) {
		bool dir = is_directory(it->path());
		stream << boost::format("%crwxrwxrwx   1 %-10s %-10s %10lu Jan  1  1970 %s\r\n") 
			% (dir ? 'd' : '-') 
			% "oriane" % "oriane" 
			% (dir ? 0 : file_size(it->path())) 
			% it->path().filename().string();
	}

	return stream.str();
}

template<typename T> struct dumper : boost::enable_shared_from_this<T> {
	boost::asio::io_service& service;
	tcp::socket socket;
	boost::function<void (const orianne::ftp_result&)> callback;

	explicit dumper(boost::function<void (const orianne::ftp_result&)> cb, boost::asio::io_service& service_)
		: service(service_), socket(service), callback(cb)
	{
	}

	static boost::shared_ptr<T> create(boost::function<void (const orianne::ftp_result&)> cb, boost::asio::io_service& service) {
		return boost::shared_ptr<T>(new T(cb, service));
	}

	void async_wait(boost::asio::ip::tcp::acceptor& acceptor) {
		acceptor.async_accept(socket,
			boost::bind(&T::handle_connect, this->shared_from_this()));
	}
};

struct dir_list_dumper : dumper<dir_list_dumper> {
	std::string data;
	
	explicit dir_list_dumper(boost::function<void (const orianne::ftp_result&)> cb, boost::asio::io_service& service)
		: dumper(cb, service)
	{
	}

	void handle_connect() {
		boost::asio::async_write(socket,
			boost::asio::buffer(data),
			boost::bind(&dir_list_dumper::handle_write, shared_from_this()));
		callback(orianne::ftp_result(150, "Sending directory listing."));
	}

	void handle_write() {
		callback(orianne::ftp_result(226, "Done."));
	}

	void set_data(const std::string& data_) {
		data = data_;
	}
};

void orianne::ftp_session::list(boost::function<void (const orianne::ftp_result&)> cb) {
	boost::shared_ptr<dir_list_dumper> dumper = dir_list_dumper::create(cb, io_service);
	dumper->set_data(get_list(root_directory / working_directory));
	dumper->async_wait(*acceptor);
}

struct file_dumper : dumper<file_dumper> {
	std::ifstream stream;
	char buffer[1024];
	boost::asio::mutable_buffers_1 m_buffer;

	explicit file_dumper(boost::function<void (const orianne::ftp_result&)> cb, boost::asio::io_service& service, const std::string& path)
		: dumper(cb, service), stream(path.c_str(), std::ios::in | std::ios::binary), m_buffer(buffer, 1024)
	{
	}

	static boost::shared_ptr<file_dumper> create(boost::function<void (const orianne::ftp_result&)> cb, boost::asio::io_service& service, const std::string& path) {
		return boost::shared_ptr<file_dumper>(new file_dumper(cb, service, path));
	}

	void handle_connect() {
		callback(orianne::ftp_result(150, "Sending file contents."));
		
		handle_write();
	}

	void handle_write() {
		stream.read(buffer, 1024);
		std::streamsize count = stream.gcount();

		if(count == 0) {
			callback(orianne::ftp_result(226, "Done."));
		} else {
			if(count < 1024)
				m_buffer = boost::asio::buffer(buffer, count);

			boost::asio::async_write(socket,
				m_buffer,
				boost::bind(&file_dumper::handle_write, shared_from_this()));
		}
	}

	~file_dumper() {
	}
};

void orianne::ftp_session::retrieve(const std::string& filename, boost::function<void (const orianne::ftp_result&)> cb) {
	boost::filesystem::path path = root_directory / working_directory / filename;
	
	std::cout << "Opening " << path.make_preferred() << " for download" << std::endl;

	boost::shared_ptr<file_dumper> dumper = file_dumper::create(cb, io_service, path.make_preferred().string());
	dumper->async_wait(*acceptor);
}

orianne::ftp_transfer_type orianne::read_transfer_type(std::istream& stream) {
	orianne::ftp_transfer_type transfer_type;
	transfer_type.type = 'I';
	return transfer_type;
}
