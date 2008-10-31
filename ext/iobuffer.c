/*
 * Copyright (C) 2007-08 Tony Arcieri
 * You may redistribute this under the terms of the MIT license.
 * See LICENSE for details
 */

#include "ruby.h"
#include "rubyio.h"

#include <assert.h>

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* Macro for retrieving the file descriptor from an FPTR */
#if !HAVE_RB_IO_T || (RUBY_VERSION_MAJOR == 1 && RUBY_VERSION_MINOR == 8)
#define FPTR_TO_FD(fptr) fileno(fptr->f)
#else
#define FPTR_TO_FD(fptr) fptr->fd
#endif

/* Default number of bytes in each node's buffer.  Should be >= MTU */
#define DEFAULT_NODE_SIZE 4096

struct buffer {
  unsigned size, node_size;
  struct buffer_node *head, *tail;
  struct buffer_node *pool_head, *pool_tail;
	
};

struct buffer_node {
  unsigned start, end;
  struct buffer_node *next;
  unsigned char data[0];
};

static VALUE cIO_Buffer = Qnil;

static VALUE IO_Buffer_allocate(VALUE klass);
static void IO_Buffer_mark(struct buffer *);
static void IO_Buffer_free(struct buffer *);

static VALUE IO_Buffer_initialize(int argc, VALUE *argv, VALUE self);
static VALUE IO_Buffer_clear(VALUE self);
static VALUE IO_Buffer_size(VALUE self);
static VALUE IO_Buffer_empty(VALUE self);
static VALUE IO_Buffer_append(VALUE self, VALUE data);
static VALUE IO_Buffer_prepend(VALUE self, VALUE data);
static VALUE IO_Buffer_read(int argc, VALUE *argv, VALUE self);
static VALUE IO_Buffer_to_str(VALUE self);
static VALUE IO_Buffer_read_from(VALUE self, VALUE io);
static VALUE IO_Buffer_write_to(VALUE self, VALUE io);

static struct buffer *buffer_new(void);
static void buffer_clear(struct buffer *buf);
static void buffer_free(struct buffer *buf);
static void buffer_free_pool(struct buffer *buf);
static void buffer_prepend(struct buffer *buf, char *str, unsigned len);
static void buffer_append(struct buffer *buf, char *str, unsigned len);
static void buffer_read(struct buffer *buf, char *str, unsigned len);
static void buffer_copy(struct buffer *buf, char *str, unsigned len);
static int buffer_read_from(struct buffer *buf, int fd);
static int buffer_write_to(struct buffer *buf, int fd);

/* 
 * High-performance I/O buffer intended for use in non-blocking programs
 *
 * Data is stored in as a memory-pooled linked list of equally sized
 * chunks.  Routines are provided for high speed non-blocking reads
 * and writes from Ruby IO objects.
 */
void Init_iobuffer()
{
  cIO_Buffer = rb_define_class_under(rb_cIO, "Buffer", rb_cObject);
  rb_define_alloc_func(cIO_Buffer, IO_Buffer_allocate);

  rb_define_method(cIO_Buffer, "initialize", IO_Buffer_initialize, -1);
  rb_define_method(cIO_Buffer, "clear", IO_Buffer_clear, 0);
  rb_define_method(cIO_Buffer, "size", IO_Buffer_size, 0);
  rb_define_method(cIO_Buffer, "empty?", IO_Buffer_empty, 0);
  rb_define_method(cIO_Buffer, "<<", IO_Buffer_append, 1);
  rb_define_method(cIO_Buffer, "append", IO_Buffer_append, 1);
  rb_define_method(cIO_Buffer, "prepend", IO_Buffer_prepend, 1);
  rb_define_method(cIO_Buffer, "read", IO_Buffer_read, -1);
	rb_define_method(cIO_Buffer, "to_str", IO_Buffer_to_str, 0);
	rb_define_method(cIO_Buffer, "read_from", IO_Buffer_read_from, 1);
  rb_define_method(cIO_Buffer, "write_to", IO_Buffer_write_to, 1);
}

static VALUE IO_Buffer_allocate(VALUE klass)
{
  return Data_Wrap_Struct(klass, IO_Buffer_mark, IO_Buffer_free, buffer_new());
}

static void IO_Buffer_mark(struct buffer *buf)
{
  /* Naively discard the memory pool whenever Ruby garbage collects */
  buffer_free_pool(buf);
}

static void IO_Buffer_free(struct buffer *buf)
{
  buffer_free(buf);
}

/**
 *  call-seq:
 *    IO_Buffer.new(size = DEFAULT_NODE_SIZE) -> IO_Buffer
 * 
 * Create a new IO_Buffer with linked segments of the given size
 */
static VALUE IO_Buffer_initialize(int argc, VALUE *argv, VALUE self)
{
  VALUE node_size_obj;
  int node_size;
  struct buffer *buf;

  if(rb_scan_args(argc, argv, "01", &node_size_obj) == 1) {
    node_size = NUM2INT(node_size_obj);

    if(node_size < 1) rb_raise(rb_eArgError, "invalid buffer size");

    Data_Get_Struct(self, struct buffer, buf);

    /* Make sure we're not changing the buffer size after data has been allocated */
    assert(!buf->head);
    assert(!buf->pool_head);

    buf->node_size = node_size;
  }

  return Qnil;
}

/**
 *  call-seq:
 *    IO_Buffer#clear -> nil
 * 
 * Clear all data from the IO_Buffer
 */
static VALUE IO_Buffer_clear(VALUE self)
{
  struct buffer *buf;
  Data_Get_Struct(self, struct buffer, buf);

  buffer_clear(buf);

  return Qnil;
}

/**
 *  call-seq:
 *    IO_Buffer#size -> Integer
 * 
 * Return the size of the buffer in bytes
 */
static VALUE IO_Buffer_size(VALUE self) 
{
  struct buffer *buf;
  Data_Get_Struct(self, struct buffer, buf);

  return INT2NUM(buf->size);
}

/**
 *  call-seq:
 *    IO_Buffer#empty? -> Boolean
 * 
 * Is the buffer empty?
 */
static VALUE IO_Buffer_empty(VALUE self) 
{
  struct buffer *buf;
  Data_Get_Struct(self, struct buffer, buf);

  return buf->size > 0 ? Qfalse : Qtrue;	
}

/**
 *  call-seq:
 *    IO_Buffer#append(data) -> String
 * 
 * Append the given data to the end of the buffer
 */
static VALUE IO_Buffer_append(VALUE self, VALUE data)
{
  struct buffer *buf;
  Data_Get_Struct(self, struct buffer, buf);

  /* Is this needed?  Never seen anyone else do it... */
  data = rb_convert_type(data, T_STRING, "String", "to_str");
  buffer_append(buf, RSTRING_PTR(data), RSTRING_LEN(data));

  return data;
}

/**
 *  call-seq:
 *    IO_Buffer#prepend(data) -> String
 * 
 * Prepend the given data to the beginning of the buffer
 */
static VALUE IO_Buffer_prepend(VALUE self, VALUE data)
{
  struct buffer *buf;
  Data_Get_Struct(self, struct buffer, buf);

  data = rb_convert_type(data, T_STRING, "String", "to_str");
  buffer_prepend(buf, RSTRING_PTR(data), RSTRING_LEN(data));

  return data;
}

/**
 *  call-seq:
 *    IO_Buffer#read(length = nil) -> String
 * 
 * Read the specified abount of data from the buffer.  If no value
 * is given the entire contents of the buffer are returned.  Any data
 * read from the buffer is cleared.
 */
static VALUE IO_Buffer_read(int argc, VALUE *argv, VALUE self)
{
  VALUE length_obj, str;
  int length;
  struct buffer *buf;

  Data_Get_Struct(self, struct buffer, buf);

  if(rb_scan_args(argc, argv, "01", &length_obj) == 1) {
    length = NUM2INT(length_obj);
  } else {
    if(buf->size == 0)
      return rb_str_new2("");

    length = buf->size;
  }

  if(length > buf->size)
    length = buf->size;

  if(length < 1)
    rb_raise(rb_eArgError, "length must be greater than zero");

  str = rb_str_new(0, length);
  buffer_read(buf, RSTRING_PTR(str), length);

  return str;
}

/**
 *  call-seq:
 *    IO_Buffer#to_str -> String
 * 
 * Convert the Buffer to a String.  The original buffer is unmodified.
 */
static VALUE IO_Buffer_to_str(VALUE self) {
	VALUE str;
	struct buffer *buf;
	
	Data_Get_Struct(self, struct buffer, buf);
	
	str = rb_str_new(0, buf->size);
	buffer_copy(buf, RSTRING_PTR(str), buf->size);
  
  return str;
}

/**
 *  call-seq:
 *    IO_Buffer#read_from(io) -> Integer
 * 
 * Perform a nonblocking read of the the given IO object and fill
 * the buffer with any data received.  The call will read as much
 * data as it can until the read would block.
 */
static VALUE IO_Buffer_read_from(VALUE self, VALUE io) {
	struct buffer *buf;
#if HAVE_RB_IO_T
  rb_io_t *fptr;
#else
  OpenFile *fptr;
#endif

  Data_Get_Struct(self, struct buffer, buf);
  GetOpenFile(rb_convert_type(io, T_FILE, "IO", "to_io"), fptr);
  rb_io_set_nonblock(fptr);

  return INT2NUM(buffer_read_from(buf, FPTR_TO_FD(fptr)));
}

/**
 *  call-seq:
 *    IO_Buffer#write_to(io) -> Integer
 * 
 * Perform a nonblocking write of the buffer to the given IO object.
 * As much data as possible is written until the call would block.
 * Any data which is written is removed from the buffer.
 */
static VALUE IO_Buffer_write_to(VALUE self, VALUE io) {
  struct buffer *buf;
#if HAVE_RB_IO_T 
  rb_io_t *fptr;
#else
  OpenFile *fptr;
#endif

  Data_Get_Struct(self, struct buffer, buf);
  GetOpenFile(rb_convert_type(io, T_FILE, "IO", "to_io"), fptr);
  rb_io_set_nonblock(fptr);

  return INT2NUM(buffer_write_to(buf, FPTR_TO_FD(fptr)));
}

/*
 * Ruby bindings end here.  Below is the actual implementation of 
 * the underlying byte queue ADT
 */

/* Create a new buffer */
static struct buffer *buffer_new(void)
{
  struct buffer *buf;

  buf = (struct buffer *)xmalloc(sizeof(struct buffer));
  buf->head = buf->tail = buf->pool_head = buf->pool_tail = 0;
  buf->size = 0;
  buf->node_size = DEFAULT_NODE_SIZE;
	
  return buf;
}

/* Clear all data from a buffer */
static void buffer_clear(struct buffer *buf)
{
  /* Move everything into the buffer pool */
  if(!buf->pool_tail)
    buf->pool_head = buf->pool_tail = buf->head;
  else
    buf->pool_tail->next = buf->head;

  buf->head = buf->tail = 0;
  buf->size = 0;
}

/* Free a buffer */
static void buffer_free(struct buffer *buf) 
{
  buffer_clear(buf);
  buffer_free_pool(buf);

  free(buf);
}

/* Free the memory pool */
static void buffer_free_pool(struct buffer *buf)
{
  struct buffer_node *tmp;
  
  while(buf->pool_head) {
    tmp = buf->pool_head;
    buf->pool_head = tmp->next;
    free(tmp);
  }
  
	buf->pool_tail = 0;
}

/* Create a new buffer_node (or pull one from the memory pool) */
static struct buffer_node *buffer_node_new(struct buffer *buf)
{
  struct buffer_node *node;

  /* Pull from the memory pool if available */
  if(buf->pool_head) {
    node = buf->pool_head;
    buf->pool_head = node->next;

    if(node->next)
      node->next = 0;
    else
      buf->pool_tail = 0;
  } else {
    node = (struct buffer_node *)xmalloc(sizeof(struct buffer_node) + buf->node_size);
    node->next = 0;
  }

  node->start = node->end = 0;
  return node;
}

/* Free a buffer node (i.e. return it to the memory pool) */
static void buffer_node_free(struct buffer *buf, struct buffer_node *node)
{
  node->next = buf->pool_head;
  buf->pool_head = node;

  if(!buf->pool_tail)
    buf->pool_tail = node;
}

/* Prepend data to the front of the buffer */
static void buffer_prepend(struct buffer *buf, char *str, unsigned len)
{
  struct buffer_node *node, *tmp;
  buf->size += len;

  /* If it fits in the beginning of the head */
  if(buf->head && buf->head->start >= len) {
    buf->head->start -= len;
    memcpy(buf->head->data + buf->head->start, str, len);
  } else {
    node = buffer_node_new(buf);
    node->next = buf->head;
    buf->head = node;
    if(!buf->tail) buf->tail = node;

    while(len > buf->node_size) {
      memcpy(node->data, str, buf->node_size);
      node->end = buf->node_size;

      tmp = buffer_node_new(buf);
      tmp->next = node->next;
      node->next = tmp;

      if(buf->tail == node) buf->tail = tmp;
      node = tmp;

      str += buf->node_size;
      len -= buf->node_size;
    }

    if(len > 0) {
      memcpy(node->data, str, len);
      node->end = len;
    }
  }
}

/* Append data to the front of the buffer */
static void buffer_append(struct buffer *buf, char *str, unsigned len)
{
  unsigned nbytes;
  buf->size += len;

  /* If it fits in the remaining space in the tail */
  if(buf->tail && len <= buf->node_size - buf->tail->end) {
    memcpy(buf->tail->data + buf->tail->end, str, len);
    buf->tail->end += len;
    return;
  }

  /* Empty list needs initialized */
  if(!buf->head) {
    buf->head = buffer_node_new(buf);
    buf->tail = buf->head;
  }

  /* Build links out of the data */
  while(len > 0) {
    nbytes = buf->node_size - buf->tail->end;
    if(len < nbytes) nbytes = len;
    
    memcpy(buf->tail->data + buf->tail->end, str, nbytes);
    str += nbytes;    
    len -= nbytes;
    
    buf->tail->end += nbytes;

    if(len > 0) {
      buf->tail->next = buffer_node_new(buf);
      buf->tail = buf->tail->next;
    }
  }
}

/* Read data from the buffer (and clear what we've read) */
static void buffer_read(struct buffer *buf, char *str, unsigned len)
{
  unsigned nbytes;
  struct buffer_node *tmp;

  while(buf->size > 0 && len > 0) {
    nbytes = buf->head->end - buf->head->start;
    if(len < nbytes) nbytes = len;

    memcpy(str, buf->head->data + buf->head->start, nbytes);
    str += nbytes;
    len -= nbytes;

    buf->head->start += nbytes;
    buf->size -= nbytes;

    if(buf->head->start == buf->head->end) {
      tmp = buf->head;
      buf->head = tmp->next;
      buffer_node_free(buf, tmp);

      if(!buf->head) buf->tail = 0;
    }
  }
}

/* Copy data from the buffer without clearing it */
static void buffer_copy(struct buffer *buf, char *str, unsigned len)
{
  unsigned nbytes;
  struct buffer_node *node;

	node = buf->head;
  while(node && len > 0) {
    nbytes = node->end - node->start;
    if(len < nbytes) nbytes = len;

    memcpy(str, node->data + node->start, nbytes);
    str += nbytes;
    len -= nbytes;

    if(node->start + nbytes == node->end)
			node = node->next;
  }
}

/* Write data from the buffer to a file descriptor */
static int buffer_write_to(struct buffer *buf, int fd)
{
  int bytes_written, total_bytes_written = 0;
  struct buffer_node *tmp;

  while(buf->head) {
    bytes_written = write(fd, buf->head->data + buf->head->start, buf->head->end - buf->head->start);

    /* If the write failed... */
    if(bytes_written < 0) {
      if(errno != EAGAIN)
        rb_sys_fail("write");

      return total_bytes_written;
    }

    total_bytes_written += bytes_written;
    buf->size -= bytes_written;

    /* If the write blocked... */
    if(bytes_written < buf->head->end - buf->head->start) {
      buf->head->start += bytes_written;
      return total_bytes_written;
    }

    /* Otherwise we wrote the whole buffer */
    tmp = buf->head;
    buf->head = tmp->next;
    buffer_node_free(buf, tmp);

    if(!buf->head) buf->tail = 0;
  }

  return total_bytes_written;
}

/* Read data from a file descriptor to a buffer */
/* Append data to the front of the buffer */
static int buffer_read_from(struct buffer *buf, int fd)
{
	int bytes_read, total_bytes_read = 0;
  unsigned nbytes;

  /* Empty list needs initialized */
  if(!buf->head) {
    buf->head = buffer_node_new(buf);
    buf->tail = buf->head;
  }

	do {
	  nbytes = buf->node_size - buf->tail->end;
		bytes_read = read(fd, buf->tail->data + buf->tail->end, nbytes);
	
		if(bytes_read < 1) {
			if(errno != EAGAIN)
        rb_sys_fail("read");
			
			return total_bytes_read;
		}
		
		total_bytes_read += bytes_read; 
		buf->tail->end += nbytes;
		buf->size += nbytes;
		
		if(buf->tail->end == buf->node_size) {
      buf->tail->next = buffer_node_new(buf);
      buf->tail = buf->tail->next;
		}
	} while(bytes_read == nbytes);
	
	return total_bytes_read;
}