#pragma once

#include "AmEvent.h"
#include "sys/time.h"

#define HTTP_EVENT_QUEUE "http"

struct HttpEvent {
  enum Type {
      Upload = 0,
      Post,
      MultiPartForm,
      TriggerSyncContext
  };

  string session_id;
  string token;
  string sync_ctx_id;
  struct timeval created_at;
  unsigned int failover_idx;
  unsigned int attempt;
  bool is_send_failed;

  HttpEvent(
      string session_id, string token, const string &sync_ctx_id = string(),
      unsigned int failover_idx = 0,
      unsigned int attempt = 0, bool is_send_failed = false)
    : session_id(session_id), token(token), sync_ctx_id(sync_ctx_id),
      failover_idx(failover_idx),
      attempt(attempt),
      is_send_failed(is_send_failed)
  {
      gettimeofday(&created_at,NULL);
  }
  virtual ~HttpEvent(){}
};


struct HttpUploadEvent
  : public HttpEvent, AmEvent
{

  string file_path;
  string file_name;
  string destination_name;

  HttpUploadEvent(string destination_name, string file_name, string file_path, string token,
                  string session_id = string(),
                  const string &sync_ctx_id = string())
    : AmEvent(Upload),
      HttpEvent(session_id,token,sync_ctx_id),
      destination_name(destination_name),
      file_name(file_name),
      file_path(file_path)
  { }

  HttpUploadEvent(const HttpUploadEvent &src)
    : AmEvent(Upload),
      HttpEvent(src.session_id,src.token,src.sync_ctx_id,src.failover_idx,src.attempt, src.is_send_failed),
      destination_name(src.destination_name),
      file_name(src.file_name),
      file_path(src.file_path)
  {}

  HttpUploadEvent *clone() {
    return new HttpUploadEvent(*this);
  }
};

struct HttpPostMultipartFormEvent
  : public HttpEvent, AmEvent
{
    struct Part {
        enum Type {
            ImmediateValue,
            FilePath
        } type;
        string name;
        string content_type;
        string value;
        Part(const string &name,
             const string &content_type,
             const string &value,
             Type type = ImmediateValue)
          : name(name),
            content_type(content_type),
            value(value),
            type(type)
        {}
    };
    vector<Part> parts;
    string destination_name;

    HttpPostMultipartFormEvent(string destination_name, string token,
                               string session_id = string(),
                               const string &sync_ctx_id = string())
      : AmEvent(MultiPartForm),
        HttpEvent(session_id,token,sync_ctx_id),
        destination_name(destination_name)
    { }

    HttpPostMultipartFormEvent(const HttpPostMultipartFormEvent &src)
      : AmEvent(MultiPartForm),
        HttpEvent(src.session_id,src.token,src.sync_ctx_id,src.failover_idx,src.attempt, src.is_send_failed),
        destination_name(src.destination_name),
        parts(src.parts)
    { }

    HttpPostMultipartFormEvent *clone() {
      return new HttpPostMultipartFormEvent(*this);
    }
};

struct HttpUploadResponseEvent
  : public AmEvent
{

  long int code;
  string token;

  HttpUploadResponseEvent(long int code, string token = string())
    : AmEvent(E_PLUGIN),
      code(code),
      token(token)
  {}
};

struct HttpPostEvent
  : public HttpEvent, AmEvent
{
  string data;
  string destination_name;

  HttpPostEvent(string destination_name, string data, string token,
                string session_id = string(),
                const string &sync_ctx_id = string())
    : AmEvent(Post),
      HttpEvent(session_id,token,sync_ctx_id),
      destination_name(destination_name),
      data(data)
  {}

  HttpPostEvent(const HttpPostEvent &src)
    : AmEvent(Post),
      HttpEvent(src.session_id,src.token,src.sync_ctx_id,src.failover_idx,src.attempt, src.is_send_failed),
      destination_name(src.destination_name),
      data(src.data)
  {}
};

struct HttpPostResponseEvent
  : public AmEvent
{

  long int code;
  string token;
  string data;

  HttpPostResponseEvent(long int code, string &data, string token = string())
    : AmEvent(E_PLUGIN),
      code(code),
      data(data),
      token(token)
  {}
};

struct HttpTriggerSyncContext
  : public AmEvent
{
  string sync_ctx_id;
  int quantity;

  HttpTriggerSyncContext(string &ctx_id, int quantity)
    : AmEvent(HttpEvent::TriggerSyncContext),
      sync_ctx_id(ctx_id),
      quantity(quantity)
  {}
};
