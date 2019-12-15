#pragma once

#include <unistd.h>
#include <iostream>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <cstring>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <boost/lexical_cast.hpp>

#include <li/http_backend/moustique.hh>
#include <li/http_backend/error.hh>
#include <li/http_backend/symbols.hh>
#include <li/http_backend/url_unescape.hh>

namespace li {  

namespace http_async_impl {  

char* date_buf = nullptr;
int date_buf_size = 0;

thread_local std::unordered_map<std::string, std::string_view> static_files;

struct read_buffer
{

  std::vector<char> buffer_;
  int cursor = 0; // First index of the currently used buffer area
  int end = 0; // Index of the last read character
  
  read_buffer()
    : buffer_(4 * 1024, 0),
      cursor(0),
      end(0)
  {}

  // Free unused space in the buffer in [i1, i2[.
  // This may move data in [i2, end[ if needed.
  // Return the
  void free(int i1, int i2)
  {
    assert(i1 < i2);
    assert(i1 >= 0 and i1 < buffer_.size());
    assert(i2 > 0 and i2 <= buffer_.size());

    if (i1 == cursor and i2 == end) // eat the whole buffer.
      cursor = end = 0;
    else if (i1 == cursor) // eat the beggining of the buffer.
      cursor = i2;
    else if (i2 == end) end = i1; // eat the end of the buffer.
    else if (i2 != end) // eat somewhere in the middle.
    {
      if (buffer_.size() - end < buffer_.size() / 4)
      {
        if (end - i2 > i2 - i1) // use memmove if overlap.
          std::memmove(buffer_.data() + i1, buffer_.data() + i2, end - i2);
        else
          std::memcpy(buffer_.data() + i1, buffer_.data() + cursor, end - cursor);
      }
    }
  }

  void free(const char* i1, const char* i2) { 
    assert(i1 >= buffer_.data());
    assert(i1 <= &buffer_.back());
    assert(i2 >= buffer_.data() and i2 <= &buffer_.back());
    free(i1 - buffer_.data(), i2 - buffer_.data()); 
  }
  void free(const std::string_view& str) { free(str.data(), str.data() + str.size()); }
  
  // private: Read more data
  // Read from ptr until character x.
  // read n more characters at address ptr.
  // eat n first/last character.
  // free part of the buffer and more data if needed.

  // Read more data.
  // Return 0 on error.
  template <typename F>
  int read_more(F&& read, int size = -1)
  {
    if (buffer_.size() == end)
    {
      if (buffer_.size() > (10*1024*1024))
        return 0; // Buffer is full. Error and return.
      else 
      {
        std::cout << "RESIZE" << std::endl;
        buffer_.resize(buffer_.size() * 2);
      }
    }

    if (size == -1) size = buffer_.size() - end;
    int received = read(buffer_.data() + end, size);
    if (received == 0) return 0; // Socket closed, return.
    end = end + received;
    return received;
  }
  template <typename F>
  std::string_view read_more_str(F&& read)
  {
    int l = read_more(read);
    return std::string_view(buffer_.data() + end - l);
  }

  template <typename F>
  std::string_view read_n(F&& read, const char* start, int size)
  {
    int str_start = start - buffer_.data();
    int str_end = size + str_start;
    if (end < str_end)
    {
      // Read more body on the socket.
      int current_size = end - str_start;
      while (current_size < size)
          current_size += read_more(read);
    }
    return std::string_view(start, size);
  }
  
  template <typename F>
  std::string_view read_until(F&& read, const char*& start, char delimiter)
  {
    const char* str_end = start;

    while (true)
    {
      const char* buffer_end = buffer_.data() + end;
      while (str_end < buffer_end and *str_end != delimiter) str_end++;

      if (*str_end == delimiter) break;
      else {
        if (!read_more(read)) break;
      }
    }

    auto res = std::string_view(start, str_end - start);
    start = str_end + 1;
    return res;
  }

  bool empty() const { return cursor == end; }

  // Return the amount of data currently available to read.
  int current_size() { return end - cursor; }

  // Reset the buffer. Copy remaining data at the beggining if there is some.
  void reset() {
    assert(cursor <= end);
    if (cursor == end)
      end = cursor = 0;
    else
    {
      if (cursor > end - cursor) // use memmove if overlap.
        std::memmove(buffer_.data(), buffer_.data() + cursor, end - cursor);
      else
        std::memcpy(buffer_.data(), buffer_.data() + cursor, end - cursor);

      // if (
      // memcpy(buffer_.data(), buffer_.data() + cursor, end - cursor);

      end = end - cursor;
      cursor = 0;
    }

  }

  // On success return the number of bytes read.
  // On error return 0.
  char* data() { return buffer_.data(); }
};

struct output_buffer
{

  output_buffer(void* buffer, int capacity)
    : buffer_((char*)buffer),
      cursor_(buffer_),
      end_(buffer_ + capacity)      
  {
    assert(buffer_);
  }

  void reset() 
  {
    cursor_ = buffer_;
  }

  output_buffer& operator<<(std::string_view s)
  {
    assert(cursor_ + s.size() < end_);
    memcpy(cursor_, s.data(), s.size());
    cursor_ += s.size();
    return *this;
  }
  output_buffer& operator<<(char v)
  {
    cursor_[0] = v;
    cursor_++;
    return *this;
  }

  // output_buffer& operator<<(int v) { return integer(v); }
  // output_buffer& operator<<(unsigned int v) { return integer(v); }
  // output_buffer& operator<<(size_t v) { return integer(v); }

  template <typename I>
  output_buffer& operator<<(I v)
  {
    typedef std::array<char, 50> buf_t;
    buf_t b = boost::lexical_cast<buf_t>(v);
    operator<<(std::string_view(b.begin(), strlen(b.begin())));
    return *this;
  }
  
  std::string_view to_string_view() { return std::string_view(buffer_, cursor_ - buffer_); }
  char* buffer_;
  char* cursor_;
  char* end_;
};

struct http_ctx {

  http_ctx(read_buffer& _rb,
           std::function<int(char*, int)> _read,
           std::function<bool(const char*, int)> _write,
           std::function<void(int)> _listen_to_new_fd
           )
    : rb(_rb),
      read(_read),
      write(_write),
      listen_to_new_fd(_listen_to_new_fd),
      headers_stream(headers_buffer_space, sizeof(headers_buffer_space))
  {
    get_parameters_map.reserve(10);
    response_headers.reserve(20);

  }

  http_ctx& operator=(const http_ctx&) = delete;
  http_ctx(const http_ctx&) = delete;

  std::string_view header(const char* key)
  {
    if (!header_map.size())
      index_headers();
    return header_map[key];
  }
  
  std::string_view cookie(const char* key)
  {
    if (!cookie_map.size())
      index_cookies();
    return cookie_map[key];
  }

  std::string_view get_parameter(const char* key)
  {
    if (!url_.size())
      parse_first_line();
    return get_parameters_map[key];
  }
  
  // std::string_view post_parameter(const char* key)
  // {
  //   if (!is_body_read_)
  //   {
  //     //read_whole_body();
  //     parse_post_parameters();
  //   }
  //   return post_parameters_map[key];
  // }
 
  // std::string_view read_body(char* buf, int buf_size)
  // {
  //   // Try to read Content-Length

  //   // if part of the body is in the request buffer
  //   //  return it and mark it as read.
  //   // otherwise call read on the socket.

  //   // handle chunked encoding here if needed.
  // }

  // void sendfile(std::string path)
  // {
  //   char buffer[10200];
  //   //std::cout << uint64_t(output_stream.buffer_) << " " << uint64_t(output_buffer_space) << std::endl;
  //   output_buffer output_stream(buffer, sizeof(buffer));

  //   struct stat stat_buf;
  //   int read_fd = open (path.c_str(), O_RDONLY);
  //   fstat (read_fd, &stat_buf);


  //   format_top_headers(output_stream);
  //   output_stream << headers_stream.to_string_view();
  //   output_stream << "Content-Length: " << size_t(stat_buf.st_size) << "\r\n\r\n"; //Add body
  //   auto m = output_stream.to_string_view();
  //   write(m.data(), m.size());
    
  //   off_t offset = 0;
  //   while (true)
  //   {
  //     int ret = ::sendfile(socket_fd, read_fd, &offset, stat_buf.st_size);
  //     if (ret == EAGAIN)
  //       write(nullptr, 0);
  //     else break;
  //   }
  // }

  std::string_view url()
  {
    if (!url_.size())
      parse_first_line();
    return url_;
  }
  std::string_view method()
  {
    if (!method_.size())
      parse_first_line();
    return method_;
  }
  std::string_view http_version()
  {
    if (!url_.size())
      parse_first_line();
    return http_version_;
  }

  inline void format_top_headers(output_buffer& output_stream)
  {
    output_stream << "HTTP/1.1 " << status_ << "\r\n";
    output_stream << "Date: " << std::string_view(date_buf, date_buf_size) << "\r\n";
    output_stream << "Connection: keep-alive\r\nServer: Moustique\r\n";
  }
  
  void prepare_request()
  {
    //parse_first_line();
    response_headers.clear();
    content_length_ = 0;
    chunked_ = 0;
    
    for (int i = 1; i < header_lines_size - 1; i++)
    {
      const char* line_end = header_lines[i + 1]; // last line is just an empty line.
      const char* cur = header_lines[i];

      if (*cur != 'C' and *cur != 'c') continue;

      std::string_view key = split(cur, line_end, ':');

      auto get_value = [&] {
        std::string_view value = split(cur, line_end, '\r');
        while (value[0] == ' ')
          value = std::string_view(value.data() + 1, value.size() - 1);
        return value;
      };

      if (key == "Content-Length")
        content_length_ = atoi(get_value().data());
      else if (key == "Content-Type")
      {
        content_type_ = get_value();
        chunked_ = (content_type_ == "chunked");
      }
      
      // header_map[key] = value;
      // std::cout << key << " -> " << value << std::endl;
    }

  }

  //void respond(std::string s) {return respond(std::string_view(s)); }

  // void respond(const char* s)
  // {
  //   return respond(std::string_view(s, strlen(s)));
  // }

  void respond(const std::string_view& s)
  {
    response_written_ = true;
    char buffer[10200];
    output_buffer output_stream(buffer, sizeof(buffer));
    if (s.size() > 10000) // writev for large content.
    {
      format_top_headers(output_stream);
      output_stream << headers_stream.to_string_view();
      output_stream << "Content-Length: " << s.size() << "\r\n\r\n"; //Add body

      auto m = output_stream.to_string_view();
      iovec iov[2];
      iov[0].iov_base = (char*)m.data();
      iov[0].iov_len = m.size();
      iov[1].iov_base = (char*)s.data();
      iov[1].iov_len = s.size();

      int total_size = 0;
      for (int i = 0; i < 1; i++) total_size += iov[i].iov_len;

      int ret = 0;
      do
      {
        ret = writev(socket_fd, iov, 2);
        if (ret == -1 and errno == EAGAIN)
          write(nullptr, 0);// yield
        assert(ret < 0 or ret == total_size);
      } while (ret == -1 and errno == EAGAIN);
    }
    else // write for small content.
     {
      format_top_headers(output_stream);
      output_stream << headers_stream.to_string_view();
      output_stream << "Content-Length: " << s.size() << "\r\n\r\n" << s; //Add body
      auto m = output_stream.to_string_view();
      write(m.data(), m.size());

      // const char* resp = "HTTP/1.1 200 OK\r\nServer: lithium\r\nDate: Fri, 13 Dec 2019 21:46:28 GMT\r\nContent-Type: Text/Plain\r\nContent-Length: 12\r\n\r\nhello world!";
      // write(resp, strlen(resp));
    }
  }

  template <typename O>
  void respond_json(const O& obj)
  {
    response_written_ = true;
    char json_buffer[10000];
    output_buffer json_stream(json_buffer, sizeof(json_buffer));

    json_encode(json_stream, obj);

    char buffer[10200];
    output_buffer output_stream(buffer, sizeof(buffer));
    format_top_headers(output_stream);
    output_stream << headers_stream.to_string_view();
    output_stream << "Content-Length: " << json_stream.to_string_view().size() << "\r\n\r\n"
                  << json_stream.to_string_view();
    auto m = output_stream.to_string_view();
    write(m.data(), m.size());
  }
  

  void respond_if_needed()
  {
    char buffer[10200];
    output_buffer output_stream(buffer, sizeof(buffer));
    if (!response_written_)
    {
      response_written_ = true;

      format_top_headers(output_stream);
      output_stream << headers_stream.to_string_view();
      output_stream << "Content-Length: 0\r\n\r\n"; //Add body
      auto m = output_stream.to_string_view();
      write(m.data(), m.size());
    }
  }

  
  void set_header(std::string_view k, std::string_view v) { 
    headers_stream << k << ": " << v << "\r\n"; 
  }

  void set_cookie(std::string_view k, std::string_view v) { 
    headers_stream << "Set-Cookie: " << k << '=' << v << "\r\n";
  }

  void set_status(int status) {

    switch (status)
    {
    case 200: status_ = "200 OK"; break;
    case 201: status_ = "201 Created"; break;
    case 204: status_ = "204 No Content"; break;
    case 304: status_ = "304 Not Modified"; break;
    case 400: status_ = "400 Bad Request"; break;
    case 401: status_ = "401 Unauthorized"; break;
    case 402: status_ = "402 Not Found"; break;
    case 403: status_ = "403 Forbidden"; break;
    case 404: status_ = "404 Not Found"; break;
    case 409: status_ = "409 Conflict"; break;
    case 500: status_ = "500 Internal Server Error"; break;
    default: status_ = "200 OK"; break;
    }
    
  }
  
  void send_static_file(const char* path)
  {
    auto it = static_files.find(path);
    if (static_files.end() == it or !it->second.size())
    {
      int fd = open(path, O_RDONLY);
      if (fd == -1)
        throw http_error::not_found("File not found.");
      int file_size = lseek(fd, (size_t)0, SEEK_END);
      auto content = std::string_view((char*)mmap(0, file_size, PROT_READ, MAP_SHARED, fd, 0), file_size);
      static_files.insert({path, content});
      respond(content);
    }
    else
      respond(it->second);
  }
  
  //private:

  void add_header_line(const char* l) { header_lines[header_lines_size++] = l; }
  const char* last_header_line() { return header_lines[header_lines_size - 1]; }

  // split a string, starting from cur and ending with split_char.
  // Advance cur to the end of the split.
  std::string_view split(const char*& cur,
                         const char* line_end, char split_char)
  {

    const char* start = cur;
    while (start < (line_end-1) and *start == split_char) start++;
    const char* end = start + 1;
    while (end < (line_end-1) and *end != split_char) end++;
    cur = end + 1;
    if (*end == split_char)
      return std::string_view(start, cur - start - 1);
    else
      return std::string_view(start, cur - start);
  }
  
  void index_headers()
  {
    for (int i = 1; i < header_lines_size - 1; i++)
    {
      const char* line_end = header_lines[i + 1]; // last line is just an empty line.
      const char* cur = header_lines[i];

      std::string_view key = split(cur, line_end, ':');
      std::string_view value = split(cur, line_end, '\r');
      while (value[0] == ' ')
        value = std::string_view(value.data() + 1, value.size() - 1);
      header_map[key] = value;
      //std::cout << key << " -> " << value << std::endl;
    }
  }

  void index_cookies()
  {
    std::string_view cookies = header("Cookie");
    if (!cookies.data()) return;
    const char* line_end = &cookies.back() + 1;
    const char* cur = &cookies.front();
    while(cur < line_end)
    {

      std::string_view key = split(cur, line_end, '=');
      std::string_view value = split(cur, line_end, ';');
      while (key[0] == ' ')
        key = std::string_view(key.data() + 1, key.size() - 1);
      cookie_map[key] = value;
    }
  }

  template <typename C>
  void url_decode_parameters(std::string_view content, C kv_callback)
  {
    const char* c = content.data();
    const char* end = c + content.size();
    if (c < end)
    {
      while (c < end)
      {
        std::string_view key = split(c, end, '=');
        std::string_view value = split(c, end, '&');
        kv_callback(key, value);
        // printf("kv: '%.*s' -> '%.*s'\n", key.size(), key.data(), value.size(), value.data());
      }
    }    
  }
    
  void parse_first_line()
  {    
    const char* c = header_lines[0];
    const char* end = header_lines[1];

    method_ = split(c, end, ' ');
    url_ = split(c, end, ' ');
    http_version_ = split(c, end, '\r');

    // url get parameters.
    c = url_.data();
    end = c + url_.size();
    url_ = split(c, end, '?');
    get_parameters_string_ = std::string_view(c, end - c);

  }

  std::string_view get_parameters_string()
  {
    if (!get_parameters_string_.data())
      parse_first_line();
    return get_parameters_string_;
  }
  template <typename F>
  void parse_get_parameters(F processor)
  {
    url_decode_parameters(get_parameters_string(), processor);
  }

  template <typename F>
  void read_body(F callback)
  {
    is_body_read_ = true;

    if (!chunked_ and !content_length_)
      body_end_ = body_start.data();
    else if (content_length_)
    {
      std::string_view res;
      int n_body_read = 0;

      // First return part of the body already in memory.
      int l = std::min(int(body_start.size()), content_length_);
      callback(std::string_view(body_start.data(), l));
      n_body_read += l;
      
      while (content_length_ > n_body_read)
      {
        std::string_view part = rb.read_more_str(read);
        int l = part.size();
        int bl = std::min(l, content_length_ - n_body_read);
        part = std::string_view(part.data(), bl);
        callback(part);
        rb.free(part);
        body_end_ = part.data();
        n_body_read += part.size();
      }

    }
    else if (chunked_)
    {
      // Chunked decoding.
      const char* cur = body_start.data();
      int chunked_size = strtol(rb.read_until(read, cur, '\r').data(), nullptr, 16);
      cur++; // skip \n
      while (chunked_size > 0)
      {
        // Read chunk.
        std::string_view chunk = rb.read_n(read, cur, chunked_size);
        callback(chunk);
        rb.free(chunk);
        cur += chunked_size + 2; // skip \r\n.

        // Read next chunk size.
        chunked_size = strtol(rb.read_until(read, cur, '\r').data(), nullptr, 16);
        cur++; // skip \n
      }
      cur += 2;// skip the terminaison chunk.
      body_end_ = cur;
      body_ = std::string_view(body_start.data(), cur - body_start.data());
    }
  }
  
  std::string_view read_whole_body()
  {
    if (!chunked_ and !content_length_)
    {
      is_body_read_ = true;
      body_end_ = body_start.data();
      return std::string_view(); // No body.
    }

    if (content_length_)
    {
      body_ = rb.read_n(read, body_start.data(), content_length_);
      body_end_ = body_.data() + content_length_;
    }
    else if (chunked_)
    {
      // Chunked decoding.
      char* out = (char*) body_start.data();
      const char* cur = body_start.data();
      int chunked_size = strtol(rb.read_until(read, cur, '\r').data(), nullptr, 16);
      cur++; // skip \n
      while (chunked_size > 0)
      {
        // Read chunk.
        std::string_view chunk = rb.read_n(read, cur, chunked_size);
        cur += chunked_size + 2; // skip \r\n.
        // Copy the body into a contiguous string.
        if (out + chunk.size() > chunk.data()) // use memmove if overlap.
          std::memmove(out, chunk.data(), chunk.size());
        else
          std::memcpy(out, chunk.data(), chunk.size());
        
        out += chunk.size();

        // Read next chunk size.
        chunked_size = strtol(rb.read_until(read, cur, '\r').data(), nullptr, 16);
        cur++; // skip \n
      }
      cur += 2;// skip the terminaison chunk.
      body_end_ = cur;
      body_ = std::string_view(body_start.data(), out - body_start.data());
    }

    is_body_read_ = true;    
    return body_;
  }

  void read_multipart_formdata()
  {

  }

  template <typename F>
  void post_iterate(F kv_callback)
  {
    if (is_body_read_) // already in memory.
      url_decode_parameters(body_, kv_callback);
    else // stream the body.
    {
      std::string_view current_key;

      read_body([] (std::string_view part) {
          // read key if needed.
          //if (!current_key.size())
          // call kv callback if some value is available.          
        });
    }
  }
  
  // Read post parameters in the body.
  std::unordered_map<std::string_view, std::string_view> post_parameters()
  {
    if (content_type_ == "application/x-www-form-urlencoded")
    {
      if (!is_body_read_)
        read_whole_body();
      url_decode_parameters(body_, [&] (auto key, auto value)
                            { post_parameters_map[key] = value; });
      return post_parameters_map;
    }
    else
    {
      // fixme: return bad request here.
    }
    return post_parameters_map;
  }

  void prepare_next_request()
  {
    if (!is_body_read_)
      read_whole_body();

    // std::cout <<"free line0: " << uint64_t(header_lines[0]) << std::endl;
    // std::cout << rb.current_size() << " " << rb.cursor << std::endl;
    rb.free(header_lines[0], body_end_);
    // std::cout << rb.current_size() << " " << rb.cursor << std::endl;
    //rb.cursor = rb.end = 0;
    //assert(rb.cursor == 0);
    headers_stream.reset();
    status_ = "200 OK";
    method_ = std::string_view();
    url_ = std::string_view();
    http_version_ = std::string_view();
    content_type_ = std::string_view();
    header_map.clear();
    cookie_map.clear();
    response_headers.clear();
    get_parameters_map.clear();
    post_parameters_map.clear();
    get_parameters_string_ = std::string_view();
    response_written_ = false;
  }

  int socket_fd;
  read_buffer& rb;

  const char* status_ = "200 OK";
  std::string_view method_;
  std::string_view url_;
  std::string_view http_version_;
  std::string_view content_type_;
  bool chunked_;
  int content_length_;
  std::unordered_map<std::string_view, std::string_view> header_map;
  std::unordered_map<std::string_view, std::string_view> cookie_map;
  std::vector<std::pair<std::string_view, std::string_view>> response_headers;
  std::unordered_map<std::string_view, std::string_view> get_parameters_map;
  std::unordered_map<std::string_view, std::string_view> post_parameters_map;
  std::string_view get_parameters_string_;
  //std::vector<std::string> strings_saver;

  bool is_body_read_ = false;
  std::string body_local_buffer_;
  std::string_view body_;
  std::string_view body_start;
  const char* body_end_ = nullptr;
  const char* header_lines[100];
  int header_lines_size = 0;
  std::function<bool(const char*, int)> write;
  std::function<int(char*, int)> read;
  std::function<void(int)> listen_to_new_fd;
  char headers_buffer_space[1000];
  output_buffer headers_stream;
  bool response_written_ = false;
};

template <typename F>
auto make_http_processor(F handler)
{
  return [handler] (int fd, auto read, auto write, auto listen_to_new_fd) {

    read_buffer rb;
    bool socket_is_valid = true;

    //http_ctx& ctx = *new http_ctx(rb, read, write);
    http_ctx ctx = http_ctx(rb, read, write, listen_to_new_fd);
    ctx.socket_fd = fd;
    //ctx.header_lines = new const char*[10];
    
    while (true)
    {
      ctx.is_body_read_ = false;
      ctx.header_lines_size = 0;
      // Read until there is a complete header.
      int header_start = rb.cursor;
      int header_end = rb.cursor;
      assert(header_start >= 0);
      assert(header_end >= 0);
      assert(ctx.header_lines_size == 0);
      ctx.add_header_line(rb.data() + header_end);
      //std::cout <<"set line0: " << uint64_t(rb.data() + header_end) << std::endl;
      assert(ctx.header_lines_size == 1);

      bool complete_header = false;
      while (!complete_header)
      {
        // Read more data from the socket.
        if (!rb.read_more(read)) return;

        // Look for end of header and save header lines.
        while (header_end < rb.end - 3)
        {
          //if (!strncmp(rb.data() + header_end, "\r\n", 2))
          if ((rb.data() + header_end)[0] == '\r' and (rb.data() + header_end)[1] == '\n')
          {
            ctx.add_header_line(rb.data() + header_end + 2);
            header_end += 2;
            if ((rb.data() + header_end)[0] == '\r' and (rb.data() + header_end)[1] == '\n')
              //if (!strncmp(rb.data() + header_end, "\r\n", 2))
            {
              complete_header = true;
              header_end += 2;
              break;
            }
          }
          else           
            header_end++;
        }
      }

      // Header is complete. Process it.
      // Run the handler.
      assert(rb.cursor <= rb.end);
      ctx.body_start = std::string_view(rb.data() + header_end, rb.end - header_end);
      ctx.prepare_request();
      handler(ctx);
      //delete[] ctx.header_lines;
      assert(rb.cursor <= rb.end);

      // Update the cursor the beginning of the next request.
      ctx.prepare_next_request();
    }
    // printf("conection lost %d.\n", fd);
    //delete &ctx;
  };

}

} // namespace http_async_impl
}


#include <li/http_backend/request.hh>
#include <li/http_backend/response.hh>

namespace li {

template <typename... O> auto http_serve(api<http_request, http_response> api, int port, O... opts) {

  auto options = mmm(opts...);

//int http_serve(int port, int nthreads, F handler)
  auto handler = [api] (http_async_impl::http_ctx& ctx) {
    http_request rq{ctx};
    http_response resp(ctx);
    try {
      api.call(ctx.method(), ctx.url(), rq, resp);
    } catch (const http_error& e) {
      ctx.set_status(e.status());
      ctx.respond(e.what());
    } catch (const std::runtime_error& e) {
      std::cerr << "INTERNAL SERVER ERROR: "<< e.what() << std::endl;
      ctx.set_status(500);
      ctx.respond("Internal server error.");
    }
    ctx.respond_if_needed();
  };

  auto date_thread = std::make_shared<std::thread>([&] () {
      char a1[100];
      char a2[100];
      memset(a1, 0, sizeof(a1));
      memset(a2, 0, sizeof(a2));
      char* date_buf_tmp1 = a1;
      char* date_buf_tmp2 = a2;
      while (true)
      {
        time_t t = time(NULL);
        const tm& tm = *gmtime(&t);
        int size = strftime(date_buf_tmp1, sizeof(a1), "%a, %d %b %Y %T GMT", &tm);
        http_async_impl::date_buf = date_buf_tmp1;
        http_async_impl::date_buf_size = size;
        std::swap(date_buf_tmp1, date_buf_tmp2);
        usleep(1e6);
      }
    });

  auto server_thread = std::make_shared<std::thread>([=] () {
    moustique_listen(port, SOCK_STREAM, 3, http_async_impl::make_http_processor(std::move(handler)));
  });

  if constexpr (has_key<decltype(options), s::non_blocking_t>())
  {
    usleep(0.1e6);
    date_thread->detach();
    server_thread->detach();
    //return mmm(s::server_thread = server_thread, s::date_thread = date_thread);
  }
  else
    server_thread->join();

}
}