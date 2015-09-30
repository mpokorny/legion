/* Copyright 2015 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Realm logging infrastructure

#include "logging.h"

#ifdef SHARED_LOWLEVEL
#define gasnet_mynode() 0
#define gasnet_nodes() 1
#else
#include "activemsg.h"
#endif

#include "cmdline.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include <set>
#include <map>

namespace Realm {

  // abstract class for an output stream
  class LoggerOutputStream {
  public:
    LoggerOutputStream(void) {}
    virtual ~LoggerOutputStream(void) {}

    virtual void write(const char *buffer, size_t len) = 0;
    virtual void flush(void) = 0;
  };

  class LoggerFileStream : public LoggerOutputStream {
  public:
    LoggerFileStream(FILE *_f, bool _close_file)
      : f(_f), close_file(_close_file)
    {}

    virtual ~LoggerFileStream(void)
    {
      if(close_file)
	fclose(f);
    }

    virtual void write(const char *buffer, size_t len)
    {
#ifndef NDEBUG
      size_t amt =
#endif
	fwrite(buffer, 1, len, f);
      assert(amt == len);
    }

    virtual void flush(void)
    {
      fflush(f);
    }

  protected:
    FILE *f;
    bool close_file;
  };

  // use a pthread mutex to prevent simultaneous writes to a stream
  template <typename T>
  class LoggerStreamSerialized : public LoggerOutputStream {
  public:
    LoggerStreamSerialized(T *_stream, bool _delete_inner)
      : stream(_stream), delete_inner(_delete_inner)
    {
#ifndef NDEBUG
      int ret =
#endif
	pthread_mutex_init(&mutex, 0);
      assert(ret == 0);
    }

    virtual ~LoggerStreamSerialized(void)
    {
#ifndef NDEBUG
      int ret =
#endif
	pthread_mutex_destroy(&mutex);
      assert(ret == 0);
      if(delete_inner)
	delete stream;
    }

    virtual void write(const char *buffer, size_t len)
    {
#ifndef NDEBUG
      int ret;
      ret =
#endif
	pthread_mutex_lock(&mutex);
      assert(ret == 0);
      stream->write(buffer, len);
#ifndef NDEBUG
      ret =
#endif
	pthread_mutex_unlock(&mutex);
      assert(ret == 0);
    }

    virtual void flush(void)
    {
#ifndef NDEBUG
      int ret;
      ret =
#endif
	pthread_mutex_lock(&mutex);
      assert(ret == 0);
      stream->flush();
#ifndef NDEBUG
      ret =
#endif
	pthread_mutex_unlock(&mutex);
      assert(ret == 0);
    }

  protected:
    LoggerOutputStream *stream;
    bool delete_inner;
    pthread_mutex_t mutex;
  };

  class LoggerConfig {
  protected:
    LoggerConfig(void);
    ~LoggerConfig(void);

  public:
    static LoggerConfig *get_config(void);

    static void flush_all_streams(void);

    void read_command_line(std::vector<std::string>& cmdline);

    // either configures a logger right away or remembers it to config once
    //   we know the desired settings
    void configure(Logger *logger);

  protected:
    bool parse_level_argument(const std::string& s);

    bool cmdline_read;
    Logger::LoggingLevel default_level;
    std::map<std::string, Logger::LoggingLevel> category_levels;
    std::string cats_enabled;
    std::set<Logger *> pending_configs;
    LoggerOutputStream *stream;
  };

  LoggerConfig::LoggerConfig(void)
    : cmdline_read(false), default_level(Logger::LEVEL_PRINT), stream(0)
  {}

  LoggerConfig::~LoggerConfig(void)
  {
    delete stream;
  }

  /*static*/ LoggerConfig *LoggerConfig::get_config(void)
  {
    static LoggerConfig cfg;
    return &cfg;
  }

  /*static*/ void LoggerConfig::flush_all_streams(void)
  {
    LoggerConfig *cfg = get_config();
    if(cfg->stream)
      cfg->stream->flush();
  }

  bool LoggerConfig::parse_level_argument(const std::string& s)
  {
    const char *p1 = s.c_str();

    while(true) {
      // skip commas
      while(*p1 == ',') p1++;
      if(!*p1) break;

      // numbers may be preceeded by name= to specify a per-category level
      std::string catname;
      if(!isdigit(*p1)) {
	const char *p2 = p1;
	while(*p2 != '=') {
	  if(!*p2) {
	    fprintf(stderr, "ERROR: category name in -level must be followed by =\n");
	    return false;
	  }
	  p2++;
	}
	catname.assign(p1, p2 - p1);
	p1 = p2 + 1;
      }

      // levels are small integers
      if(isdigit(*p1)) {
	char *p2;
	assert(errno == 0); // no leftover errors from elsewhere
	long v = strtol(p1, &p2, 10);

	if((errno == 0) && ((*p2 == 0) || (*p2) == ',') &&
	   (v >= 0) && (v <= Logger::LEVEL_NONE)) {
	  if(catname.empty())
	    default_level = (Logger::LoggingLevel)v;
	  else
	    category_levels[catname] = (Logger::LoggingLevel)v;

	  p1 = p2;
	  continue;
	}
      }

      fprintf(stderr, "ERROR: logger level malformed or out of range: '%s'\n", p1);
      return false;
    }

    return true;
  }

  void LoggerConfig::read_command_line(std::vector<std::string>& cmdline)
  {
    std::string logname;

    bool ok = CommandLineParser()
      .add_option_string("-cat", cats_enabled)
      .add_option_string("-logfile", logname)
      .add_option_method("-level", this, &LoggerConfig::parse_level_argument)
      .parse_command_line(cmdline);

    if(!ok) {
      fprintf(stderr, "couldn't parse logger config options\n");
      exit(1);
    }

    // lots of choices for log output
    if(logname.empty() || (logname == "stdout")) {
      stream = new LoggerStreamSerialized<LoggerFileStream>(new LoggerFileStream(stdout, false),
							    true);
    } else if(logname == "stderr") {
      stream = new LoggerStreamSerialized<LoggerFileStream>(new LoggerFileStream(stderr, false),
							    true);
    } else {
      // we're going to open a file, but key off a + for appending and
      //  look for a % for node number insertion
      bool append = false;
      size_t start = 0;

      if(logname[0] == '+') {
	append = true;
	start++;
      }

      FILE *f = 0;
      size_t pct = logname.find_first_of('%', start);
      if(pct == std::string::npos) {
	// no node number - everybody uses the same file
	if(gasnet_nodes() > 1) {
	  if(!append) {
	    if(gasnet_mynode() == 1)
	      fprintf(stderr, "WARNING: all ranks are logging to the same output file - appending is forced and output may be jumbled\n");
	    append = true;
	  }
	}
	const char *fn = logname.c_str() + start;
	f = fopen(fn, append ? "a" : "w");
	if(!f) {
	  fprintf(stderr, "could not open log file '%s': %s\n", fn, strerror(errno));
	  exit(1);
	}
      } else {
	// replace % with node number
	char filename[256];
	sprintf(filename, "%.*s%d%s",
		(int)(pct - start), logname.c_str() + start, gasnet_mynode(), logname.c_str() + pct + 1);

	f = fopen(filename, append ? "a" : "w");
	if(!f) {
	  fprintf(stderr, "could not open log file '%s': %s\n", filename, strerror(errno));
	  exit(1);
	}
      }
      // TODO: consider buffering in some cases?
      setbuf(f, 0); // disable output buffering
      stream = new LoggerStreamSerialized<LoggerFileStream>(new LoggerFileStream(f, true),
							    true);
    }

    atexit(LoggerConfig::flush_all_streams);

    cmdline_read = true;
    if(!pending_configs.empty()) {
      for(std::set<Logger *>::iterator it = pending_configs.begin();
	  it != pending_configs.end();
	  it++)
	configure(*it);
      pending_configs.clear();
    }
  }

  void LoggerConfig::configure(Logger *logger)
  {
    // if we haven't read the command line yet, remember this for later
    if(!cmdline_read) {
      pending_configs.insert(logger);
      return;
    }

    // see if this logger is one of the categories we want
    if(!cats_enabled.empty()) {
      bool found = false;
      const char *p = cats_enabled.c_str();
      int l = logger->get_name().length();
      const char *n = logger->get_name().c_str();
      while(*p) {
	if(((p[l] == '\0') || (p[l] == ',')) && !strncmp(p, n, l)) {
	  found = true;
	  break;
	}
	// skip to after next comma
	while(*p && (*p != ',')) p++;
	while(*p && (*p == ',')) p++;
      }
      if(!found) {
	//printf("'%s' not in '%s'\n", n, cats_enabled);
	return;
      }
    }

    // see if the level for this category has been customized
    Logger::LoggingLevel level = default_level;
    std::map<std::string, Logger::LoggingLevel>::const_iterator it = category_levels.find(logger->get_name());
    if(it != category_levels.end())
      level = it->second;

    // give this logger a copy of the global stream
    logger->add_stream(stream, level, 
		       false,  /* don't delete */
		       false); /* don't flush each write */
  }

  ////////////////////////////////////////////////////////////////////////
  //
  // class Logger

  Logger::Logger(const std::string& _name)
    : name(_name), log_level(LEVEL_NONE)
  {
    LoggerConfig::get_config()->configure(this);
  }

  Logger::~Logger(void)
  {
    // go through our streams and delete any we're supposed to
    for(std::vector<LogStream>::iterator it = streams.begin();
	it != streams.end();
	it++)
      if(it->delete_when_done)
	delete it->s;

    streams.clear();
  }

  /*static*/ void Logger::configure_from_cmdline(std::vector<std::string>& cmdline)
  {
    LoggerConfig::get_config()->read_command_line(cmdline);
  }

  void Logger::log_msg(LoggingLevel level, const std::string& msg)
  {
    // no logging of empty messages
    if(msg.length() == 0)
      return;

    // build message string, including prefix
    static const int MAXLEN = 4096;
    char buffer[MAXLEN];
    int len = snprintf(buffer, MAXLEN - 2, "[%d - %lx] {%d}{%s}: ",
		       gasnet_mynode(), (unsigned long)pthread_self(),
		       level, name.c_str());
    int amt = msg.length();
    if((len + amt) >= MAXLEN)
      amt = MAXLEN - 2 - len;
    memcpy(buffer + len, msg.data(), amt);
    len += amt;
    buffer[len++] = '\n';
    buffer[len] = 0;
    
    // go through all the streams
    for(std::vector<LogStream>::iterator it = streams.begin();
	it != streams.end();
	it++) {
      if(level < it->min_level)
	continue;

      it->s->write(buffer, len);

      if(it->flush_each_write)
	it->s->flush();
    }
  }

  void Logger::add_stream(LoggerOutputStream *s, LoggingLevel min_level,
			  bool delete_when_done, bool flush_each_write)
  {
    LogStream ls;
    ls.s = s;
    ls.min_level = min_level;
    ls.delete_when_done = delete_when_done;
    ls.flush_each_write = flush_each_write;
    streams.push_back(ls);

    // update our logging level if needed
    if(log_level > min_level)
      log_level = min_level;
  }

  ////////////////////////////////////////////////////////////////////////
  //
  // class LoggerMessage

  LoggerMessage& LoggerMessage::vprintf(const char *fmt, va_list args)
  {
    if(active) {
      char msg[256];
      vsnprintf(msg, 256, fmt, args);
      oss << msg;
    }
    return *this;
  }

}; // namespace Realm
