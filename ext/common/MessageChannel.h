/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2010 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_MESSAGE_CHANNEL_H_
#define _PASSENGER_MESSAGE_CHANNEL_H_

#include <oxt/system_calls.hpp>
#include <oxt/macros.hpp>

#include <algorithm>
#include <string>
#include <list>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <cstdarg>
#include <cmath>
#if !APR_HAVE_IOVEC
	// We don't want apr_want.h to redefine 'struct iovec'.
	// http://groups.google.com/group/phusion-passenger/browse_thread/thread/7e162f60df212e9c
	#undef APR_HAVE_IOVEC
	#define APR_HAVE_IOVEC 1
#endif

#include "StaticString.h"
#include "Exceptions.h"
#include "Utils/Timer.h"
#include "Utils/MemZeroGuard.h"
#include "Utils/IOUtils.h"

namespace Passenger {

using namespace std;
using namespace oxt;


/**
 * Convenience class for I/O operations on file descriptors.
 *
 * This class provides convenience methods for:
 *  - sending and receiving raw data over a file descriptor.
 *  - sending and receiving messages over a file descriptor.
 *  - file descriptor passing over a Unix socket.
 *  - data size limit enforcement and time constraint enforcement.
 * All of these methods use exceptions for error reporting.
 *
 * There are two kinds of messages:
 *  - Array messages. These are just a list of strings, and the message
 *    itself has a specific length. The contained strings may not
 *    contain NUL characters (<tt>'\\0'</tt>). Note that an array message
 *    must have at least one element.
 *  - Scalar messages. These are byte strings which may contain arbitrary
 *    binary data. Scalar messages also have a specific length.
 * The protocol is designed to be low overhead, easy to implement and
 * easy to parse.
 *
 * MessageChannel is to be wrapped around a file descriptor. For example:
 * @code
 *    int p[2];
 *    pipe(p);
 *    MessageChannel channel1(p[0]);
 *    MessageChannel channel2(p[1]);
 *    
 *    // Send an array message.
 *    channel2.write("hello", "world !!", NULL);
 *    list<string> args;
 *    channel1.read(args);    // args now contains { "hello", "world !!" }
 *
 *    // Send a scalar message.
 *    channel2.writeScalar("some long string which can contain arbitrary binary data");
 *    string str;
 *    channel1.readScalar(str);
 * @endcode
 *
 * The life time of a MessageChannel is independent from that of the
 * wrapped file descriptor. If a MessageChannel object is destroyed,
 * the file descriptor is not automatically closed. Call close()
 * if you want to close the file descriptor.
 *
 * @note I/O operations are not buffered.
 * @note Be careful with mixing the sending/receiving of array messages,
 *    scalar messages and file descriptors. If you send a collection of any
 *    of these in a specific order, then the receiving side must receive them
 *    in the exact some order. So suppose you first send a message, then a
 *    file descriptor, then a scalar, then the receiving side must first
 *    receive a message, then a file descriptor, then a scalar. If the
 *    receiving side does things in the wrong order then bad things will
 *    happen.
 * @note MessageChannel is not thread-safe, but is reentrant.
 * @note Some methods throw SecurityException and TimeoutException. When these
 *    exceptions are thrown, the channel will be left in an inconsistent state
 *    because only parts of the data have been read. You should close the channel
 *    after having caught these exceptions.
 *
 * @ingroup Support
 */
class MessageChannel {
private:
	const static char DELIMITER = '\0';
	int fd;
	
	#ifdef __OpenBSD__
		typedef u_int32_t uint32_t;
		typedef u_int16_t uint16_t;
	#endif

public:
	/**
	 * Construct a new MessageChannel with no underlying file descriptor.
	 * Thus the resulting MessageChannel object will not be usable.
	 * This constructor exists to allow one to declare an "empty"
	 * MessageChannel variable which is to be initialized later.
	 */
	MessageChannel() {
		this->fd = -1;
	}

	/**
	 * Construct a new MessageChannel with the given file descriptor.
	 */
	MessageChannel(int fd) {
		this->fd = fd;
	}
	
	/**
	 * Returns the underlying file descriptor. -1 if it has already been closed.
	 */
	int filenum() const {
		return fd;
	}
	
	/**
	 * Returns whether close() has been called.
	 */
	bool connected() const {
		return fd != -1;
	}
	
	/**
	 * Close the underlying file descriptor. If this method is called multiple
	 * times, the file descriptor will only be closed the first time.
	 *
	 * @throw SystemException
	 * @throw boost::thread_interrupted
	 * @post filenum() == -1
	 * @post !connected()
	 */
	void close() {
		if (fd != -1) {
			int ret = syscalls::close(fd);
			fd = -1;
			if (ret == -1) {
				throw SystemException("Cannot close file descriptor", errno);
			}
		}
	}

	/**
	 * Send an array message, which consists of the given elements, over the underlying
	 * file descriptor.
	 *
	 * @param args An object which contains the message elements. This object must
	 *             support STL-style iteration, and each iterator must have an
	 *             std::string as value. Use the StringArrayType and
	 *             StringArrayConstIteratorType template parameters to specify the exact type names.
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @pre None of the message elements may contain a NUL character (<tt>'\\0'</tt>).
	 * @see read(), write(const char *, ...)
	 */
	template<typename StringArrayType, typename StringArrayConstIteratorType>
	void write(const StringArrayType &args) {
		StringArrayConstIteratorType it;
		string data;
		uint16_t dataSize = 0;

		for (it = args.begin(); it != args.end(); it++) {
			dataSize += it->size() + 1;
		}
		data.reserve(dataSize + sizeof(dataSize));
		dataSize = htons(dataSize);
		data.append((const char *) &dataSize, sizeof(dataSize));
		for (it = args.begin(); it != args.end(); it++) {
			data.append(*it);
			data.append(1, DELIMITER);
		}
		
		writeExact(fd, data);
	}
	
	/**
	 * Send an array message, which consists of the given elements, over the underlying
	 * file descriptor.
	 *
	 * @param args The message elements.
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @pre None of the message elements may contain a NUL character (<tt>'\\0'</tt>).
	 * @see read(), write(const char *, ...)
	 */
	void write(const list<string> &args) {
		write<list<string>, list<string>::const_iterator>(args);
	}
	
	/**
	 * Send an array message, which consists of the given elements, over the underlying
	 * file descriptor.
	 *
	 * @param args The message elements.
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @pre None of the message elements may contain a NUL character (<tt>'\\0'</tt>).
	 * @see read(), write(const char *, ...)
	 */
	void write(const vector<string> &args) {
		write<vector<string>, vector<string>::const_iterator>(args);
	}
	
	/**
	 * Send an array message, which consists of the given strings, over the underlying
	 * file descriptor. Like <tt>write(const char *name, ...)</tt> but takes a va_list
	 * instead.
	 *
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @pre None of the message elements may contain a NUL character (<tt>'\\0'</tt>).
	 */
	void write(const char *name, va_list &ap) {
		list<string> args;
		args.push_back(name);
		
		while (true) {
			const char *arg = va_arg(ap, const char *);
			if (arg == NULL) {
				break;
			} else {
				args.push_back(arg);
			}
		}
		write(args);
	}
	
	/**
	 * Send an array message, which consists of the given strings, over the underlying
	 * file descriptor.
	 *
	 * @param name The first element of the message to send.
	 * @param ... Other elements of the message. These *must* be strings, i.e. of type char*.
	 *            It is also required to terminate this list with a NULL.
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @pre None of the message elements may contain a NUL character (<tt>'\\0'</tt>).
	 * @see read(), write(const list<string> &)
	 */
	void write(const char *name, ...) {
		va_list ap;
		va_start(ap, name);
		try {
			write(name, ap);
			va_end(ap);
		} catch (...) {
			va_end(ap);
			throw;
		}
	}
	
	/**
	 * Write a 32-bit big-endian unsigned integer to the underlying file descriptor.
	 *
	 * @throws SystemException An error occurred while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 */
	void writeUint32(unsigned int value) {
		uint32_t l = htonl(value);
		writeExact(fd, &l, sizeof(uint32_t));
	}
	
	/**
	 * Write a scalar message to the underlying file descriptor.
	 *
	 * @note Security guarantee: this method will not copy the data in memory,
	 *       so it's safe to use this method to write passwords to the underlying
	 *       file descriptor.
	 *
	 * @param str The scalar message's content.
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @see readScalar(), writeScalar(const char *, unsigned int)
	 */
	void writeScalar(const string &str) {
		writeScalar(str.c_str(), str.size());
	}
	
	/**
	 * Write a scalar message to the underlying file descriptor.
	 *
	 * @note Security guarantee: this method will not copy the data in memory,
	 *       so it's safe to use this method to write passwords to the underlying
	 *       file descriptor.
	 *
	 * @param data The scalar message's content.
	 * @param size The number of bytes in <tt>data</tt>.
	 * @pre <tt>data != NULL</tt>
	 * @throws SystemException An error occured while writing the data to the file descriptor.
	 * @throws boost::thread_interrupted
	 * @see readScalar(), writeScalar(const string &)
	 */
	void writeScalar(const char *data, unsigned int size) {
		writeUint32(size);
		writeExact(fd, data, size);
	}
	
	/**
	 * Pass a file descriptor. This only works if the underlying file
	 * descriptor is a Unix socket.
	 *
	 * @param fileDescriptor The file descriptor to pass.
	 * @param negotiate See Ruby's MessageChannel#send_io method's comments.
	 * @throws SystemException Something went wrong during file descriptor passing.
	 * @throws boost::thread_interrupted
	 * @pre <tt>fileDescriptor >= 0</tt>
	 * @see readFileDescriptor()
	 */
	void writeFileDescriptor(int fileDescriptor, bool negotiate = true) {
		// See message_channel.rb for more info about negotiation.
		if (negotiate) {
			vector<string> args;
			
			if (!read(args)) {
				throw IOException("Unexpected end of stream encountered while pre-negotiating a file descriptor");
			} else if (args.size() != 1 || args[0] != "pass IO") {
				throw IOException("FD passing pre-negotiation message expected.");
			}
		}
		
		struct msghdr msg;
		struct iovec vec;
		char dummy[1];
		#if defined(__APPLE__) || defined(__SOLARIS__) || defined(__arm__)
			struct {
				struct cmsghdr header;
				int fd;
			} control_data;
		#else
			char control_data[CMSG_SPACE(sizeof(int))];
		#endif
		struct cmsghdr *control_header;
		int ret;
	
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
	
		/* Linux and Solaris require msg_iov to be non-NULL. */
		dummy[0]       = '\0';
		vec.iov_base   = dummy;
		vec.iov_len    = sizeof(dummy);
		msg.msg_iov    = &vec;
		msg.msg_iovlen = 1;
	
		msg.msg_control    = (caddr_t) &control_data;
		msg.msg_controllen = sizeof(control_data);
		msg.msg_flags      = 0;
		
		control_header = CMSG_FIRSTHDR(&msg);
		control_header->cmsg_level = SOL_SOCKET;
		control_header->cmsg_type  = SCM_RIGHTS;
		#if defined(__APPLE__) || defined(__SOLARIS__) || defined(__arm__)
			control_header->cmsg_len = sizeof(control_data);
			control_data.fd = fileDescriptor;
		#else
			control_header->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(control_header), &fileDescriptor, sizeof(int));
		#endif
		
		ret = syscalls::sendmsg(fd, &msg, 0);
		if (ret == -1) {
			throw SystemException("Cannot send file descriptor with sendmsg()", errno);
		}
		
		if (negotiate) {
			vector<string> args;
			
			if (!read(args)) {
				throw IOException("Unexpected end of stream encountered while post-negotiating a file descriptor");
			} else if (args.size() != 1 || args[0] != "got IO") {
				throw IOException("FD passing post-negotiation message expected.");
			}
		}
	}
	
	/**
	 * Read an array message from the underlying file descriptor.
	 *
	 * @param args The message will be put in this variable.
	 * @return Whether end-of-file has been reached. If so, then the contents
	 *         of <tt>args</tt> will be undefined.
	 * @throws SystemException If an error occured while receiving the message.
	 * @throws boost::thread_interrupted
	 * @see write()
	 */
	bool read(vector<string> &args) {
		uint16_t size;
		int ret;
		unsigned int alreadyRead = 0;
		
		do {
			ret = syscalls::read(fd, (char *) &size + alreadyRead, sizeof(size) - alreadyRead);
			if (ret == -1) {
				throw SystemException("read() failed", errno);
			} else if (ret == 0) {
				return false;
			}
			alreadyRead += ret;
		} while (alreadyRead < sizeof(size));
		size = ntohs(size);
		
		string buffer;
		args.clear();
		buffer.reserve(size);
		while (buffer.size() < size) {
			char tmp[1024 * 8];
			ret = syscalls::read(fd, tmp, min(size - buffer.size(), sizeof(tmp)));
			if (ret == -1) {
				throw SystemException("read() failed", errno);
			} else if (ret == 0) {
				return false;
			}
			buffer.append(tmp, ret);
		}
		
		if (!buffer.empty()) {
			string::size_type start = 0, pos;
			const string &const_buffer(buffer);
			while ((pos = const_buffer.find('\0', start)) != string::npos) {
				args.push_back(const_buffer.substr(start, pos - start));
				start = pos + 1;
			}
		}
		return true;
	}
	
	/**
	 * Read a 32-bit big-endian unsigned integer from the underlying file descriptor.
	 *
	 * @param value Upon success, the read value will be stored in here.
	 * @param timeout A pointer to an integer, representing the maximum number of
	 *                milliseconds to spend on reading the entire integer.
	 *                A TimeoutException will be thrown if the timeout expires.
	 *                If no exception is thrown, the the amount of time spent on waiting
	 *                will be deducted from <tt>*timeout</tt>.
	 *                Pass NULL if you do not want to enforce any time limits.
	 * @return True if a value was read, false if EOF was reached before all data can be
	 *         read.
	 * @throws SystemException An error occurred while reading data from the file descriptor.
	 * @throws boost::thread_interrupted
	 */
	bool readUint32(unsigned int &value, unsigned long long *timeout = NULL) {
		uint32_t temp;
		
		if (!readRaw(&temp, sizeof(uint32_t), timeout)) {
			return false;
		} else {
			value = ntohl(temp);
			return true;
		}
	}
	
	/**
	 * Read a scalar message from the underlying file descriptor.
	 *
	 * @param output The message will be put in here.
	 * @param maxSize The maximum number of bytes that may be read. If the
	 *                scalar to read is larger than this, then a SecurityException
	 *                will be thrown. Set to 0 for no size limit.
	 * @param timeout A pointer to an integer, representing the maximum number of
	 *                milliseconds to spend on reading the entire scalar.
	 *                A TimeoutException will be thrown if unable to read the entire
	 *                scalar within this time period.
	 *                If no exception is thrown, the the amount of time spent on waiting
	 *                will be deducted from <tt>*timeout</tt>.
	 *                Pass NULL if you do not want to enforce any time limits.
	 * @returns Whether end-of-file was reached during reading.
	 * @throws SystemException An error occured while reading data from the file descriptor.
	 * @throws SecurityException There is more data to read than allowed by maxSize.
	 * @throws TimeoutException Unable to read the entire scalar within <tt>timeout</tt>
	 *                          milliseconds.
	 * @throws boost::thread_interrupted
	 * @see writeScalar()
	 */
	bool readScalar(string &output, unsigned int maxSize = 0, unsigned long long *timeout = NULL) {
		unsigned int size;
		unsigned int remaining;
		
		if (!readUint32(size, timeout)) {
			return false;
		}
		
		if (maxSize != 0 && size > maxSize) {
			throw SecurityException("There is more data available than is allowed by the size limit.");
		}
		
		output.clear();
		output.reserve(size);
		remaining = size;
		if (OXT_LIKELY(remaining > 0)) {
			char buf[1024 * 32];
			// Wipe the buffer when we're done; it might contain sensitive data.
			MemZeroGuard g(buf, sizeof(buf));
			
			while (remaining > 0) {
				unsigned int blockSize = min((unsigned int) sizeof(buf), remaining);
				
				if (!readRaw(buf, blockSize, timeout)) {
					return false;
				}
				output.append(buf, blockSize);
				remaining -= blockSize;
			}
		}
		return true;
	}
	
	/**
	 * Read exactly <tt>size</tt> bytes of data from the underlying file descriptor,
	 * and put the result in <tt>buf</tt>. If end-of-file has been reached, or if
	 * end-of-file was encountered before <tt>size</tt> bytes have been read, then
	 * <tt>false</tt> will be returned. Otherwise (i.e. if the read was successful),
	 * <tt>true</tt> will be returned.
	 *
	 * @param buf The buffer to place the read data in. This buffer must be at least
	 *            <tt>size</tt> bytes long.
	 * @param size The number of bytes to read.
	 * @param timeout A pointer to an integer, which specifies the maximum number of
	 *                milliseconds that may be spent on reading the <tt>size</tt> bytes
	 *                of data. If the timeout expired then TimeoutException will be
	 *                thrown.
	 *                If this function returns without throwing an exception, then the
	 *                total number of milliseconds spent on reading will be deducted
	 *                from <tt>timeout</tt>.
	 *                Pass NULL if you do not want to enforce a timeout.
	 * @return Whether reading was successful or whether EOF was reached.
	 * @pre buf != NULL
	 * @throws SystemException Something went wrong during reading.
	 * @throws TimeoutException Unable to read <tt>size</tt> bytes of data within
	 *                          <tt>timeout</tt> milliseconds.
	 * @throws boost::thread_interrupted
	 */
	bool readRaw(void *buf, unsigned int size, unsigned long long *timeout = NULL) {
		if (timeout != NULL) {
			unsigned long long t = *timeout * 1000;
			unsigned int ret;
			try {
				ret = Passenger::readExact(fd, buf, size, &t);
				*timeout = llroundl((long double) t / 1000);
				return ret == size;
			} catch (...) {
				*timeout = llroundl((long double) t / 1000);
				throw;
			}
		} else {
			return Passenger::readExact(fd, buf, size) == size;
		}
	}
	
	/**
	 * Receive a file descriptor, which had been passed over the underlying
	 * file descriptor.
	 *
	 * @param negotiate See Ruby's MessageChannel#send_io method's comments.
	 * @return The passed file descriptor.
	 * @throws SystemException If something went wrong during the
	 *            receiving of a file descriptor. Perhaps the underlying
	 *            file descriptor isn't a Unix socket.
	 * @throws IOException Whatever was received doesn't seem to be a
	 *            file descriptor.
	 * @throws boost::thread_interrupted
	 */
	int readFileDescriptor(bool negotiate = true) {
		// See message_channel.rb for more info about negotiation.
		if (negotiate) {
			write("pass IO", NULL);
		}
		
		struct msghdr msg;
		struct iovec vec;
		char dummy[1];
		#if defined(__APPLE__) || defined(__SOLARIS__) || defined(__arm__)
			// File descriptor passing macros (CMSG_*) seem to be broken
			// on 64-bit MacOS X. This structure works around the problem.
			struct {
				struct cmsghdr header;
				int fd;
			} control_data;
			#define EXPECTED_CMSG_LEN sizeof(control_data)
		#else
			char control_data[CMSG_SPACE(sizeof(int))];
			#define EXPECTED_CMSG_LEN CMSG_LEN(sizeof(int))
		#endif
		struct cmsghdr *control_header;
		int ret;

		msg.msg_name    = NULL;
		msg.msg_namelen = 0;
		
		dummy[0]       = '\0';
		vec.iov_base   = dummy;
		vec.iov_len    = sizeof(dummy);
		msg.msg_iov    = &vec;
		msg.msg_iovlen = 1;

		msg.msg_control    = (caddr_t) &control_data;
		msg.msg_controllen = sizeof(control_data);
		msg.msg_flags      = 0;
		
		ret = syscalls::recvmsg(fd, &msg, 0);
		if (ret == -1) {
			throw SystemException("Cannot read file descriptor with recvmsg()", errno);
		}
		
		control_header = CMSG_FIRSTHDR(&msg);
		if (control_header == NULL) {
			throw IOException("No valid file descriptor received.");
		}
		if (control_header->cmsg_len   != EXPECTED_CMSG_LEN
		 || control_header->cmsg_level != SOL_SOCKET
		 || control_header->cmsg_type  != SCM_RIGHTS) {
			throw IOException("No valid file descriptor received.");
		}
		
		#if defined(__APPLE__) || defined(__SOLARIS__) || defined(__arm__)
			int fd = control_data.fd;
		#else
			int fd = *((int *) CMSG_DATA(control_header));
		#endif
		
		if (negotiate) {
			try {
				write("got IO", NULL);
			} catch (...) {
				this_thread::disable_syscall_interruption dsi;
				syscalls::close(fd);
				throw;
			}
		}
		
		return fd;
	}
	
	/**
	 * Set the timeout value for reading data from this channel.
	 * If no data can be read within the timeout period, then a
	 * SystemException will be thrown by one of the read methods,
	 * with error code EAGAIN or EWOULDBLOCK.
	 *
	 * @param msec The timeout, in milliseconds. If 0 is given,
	 *             there will be no timeout.
	 * @throws SystemException Cannot set the timeout.
	 */
	void setReadTimeout(unsigned int msec) {
		// See the comment for setWriteTimeout().
		struct timeval tv;
		int ret;
		
		tv.tv_sec = msec / 1000;
		tv.tv_usec = msec % 1000 * 1000;
		ret = syscalls::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
			&tv, sizeof(tv));
		#ifndef __SOLARIS__
			// SO_RCVTIMEO is unimplemented and returns an error on Solaris
			// 9 and 10 SPARC.  Seems to work okay without it.
			if (ret == -1) {
				throw SystemException("Cannot set read timeout for socket", errno);
			}
		#endif
	}
	
	/**
	 * Set the timeout value for writing data to this channel.
	 * If no data can be written within the timeout period, then a
	 * SystemException will be thrown, with error code EAGAIN or
	 * EWOULDBLOCK.
	 *
	 * @param msec The timeout, in milliseconds. If 0 is given,
	 *             there will be no timeout.
	 * @throws SystemException Cannot set the timeout.
	 */
	void setWriteTimeout(unsigned int msec) {
		// People say that SO_RCVTIMEO/SO_SNDTIMEO are unreliable and
		// not well-implemented on all platforms.
		// http://www.developerweb.net/forum/archive/index.php/t-3439.html
		// That's why we use APR's timeout facilities as well (see Hooks.cpp).
		struct timeval tv;
		int ret;
		
		tv.tv_sec = msec / 1000;
		tv.tv_usec = msec % 1000 * 1000;
		ret = syscalls::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
			&tv, sizeof(tv));
		#ifndef __SOLARIS__
			// SO_SNDTIMEO is unimplemented and returns an error on Solaris
			// 9 and 10 SPARC.  Seems to work okay without it.
			if (ret == -1) {
				throw SystemException("Cannot set read timeout for socket", errno);
			}
		#endif
	}
};

} // namespace Passenger

#endif /* _PASSENGER_MESSAGE_CHANNEL_H_ */
