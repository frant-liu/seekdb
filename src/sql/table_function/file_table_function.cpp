/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/table_function.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/parser/parse_node.h"
#include "common/json_type/ob_json_tree.h"
#include "share/json/ob_json_helper.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <functional>
#include <iomanip>
#include <limits.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
enum FileType { NIL, BOOL, INT, REAL, TEXT, DATE, DATETIME };
struct FileColumn { std::string name; FileType type = NIL; bool nullable = false; int scale = 0; };
struct FileValue { std::string text; bool null = false; bool quoted = false; FileType type = NIL; bool temporal_encoded=false; int64_t temporal_value=0; };
struct FileOptions {
  std::string path, format, nullstr;
  bool header = true, blank = true;
  int delimiter = ',', quote = '"', escape = '"';
  int64_t skip = 0;
  uint64_t device = 0, inode = 0;
  int64_t size = 0, sec = 0, nsec = 0;
  std::vector<FileColumn> columns;
};
ObString str(const std::string &s) { return ObString(s.size(), s.data()); }
int error(const char *message, const std::string &path = {}, int64_t line = 0)
{
  std::string detail = "file SQL: " + std::string(message);
  if (!path.empty()) detail += " (" + path + ":" + std::to_string(line) + ")";
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, detail.c_str());
  return OB_INVALID_ARGUMENT;
}
std::string lower(std::string s)
{ for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
bool integer(const std::string &s, int64_t &out)
{
  if (s.empty()) return false;
  size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
  if (i == s.size()) return false;
  for (; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
  errno = 0; char *end = nullptr;
  out = strtoll(s.c_str(), &end, 10);
  return errno != ERANGE && end == s.data() + s.size();
}
bool real(const std::string &s, double &out)
{
  if (s.empty() || s.find_first_not_of("+-0123456789.eE") != std::string::npos) return false;
  errno = 0; char *end = nullptr; out = strtod(s.c_str(), &end);
  return errno != ERANGE && end == s.data() + s.size() && std::isfinite(out);
}
// Gregorian civil date conversion, independent of session timezone and libc TZ.
bool temporal(const std::string &s, int64_t &micros, bool &date_only)
{
  date_only = s.size() == 10;
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
  auto number = [&](size_t p, size_t n, int &v) {
    v = 0; if (p + n > s.size()) return false;
    for (size_t i = p; i < p+n; ++i) { if (s[i] < '0' || s[i] > '9') return false; v = 10*v+s[i]-'0'; }
    return true;
  };
  int y,m,d,h=0,mi=0,se=0;
  if (!number(0,4,y) || !number(5,2,m) || !number(8,2,d) || y < 1 || m < 1 || m > 12) return false;
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = y%4 == 0 && (y%100 != 0 || y%400 == 0);
  if (d < 1 || d > days[m-1] + (m == 2 && leap)) return false;
  int64_t fraction = 0;
  if (!date_only) {
    if (s.size() < 19 || (s[10]!=' ' && s[10]!='T') || s[13]!=':' || s[16]!=':' ||
        !number(11,2,h) || !number(14,2,mi) || !number(17,2,se) || h>23 || mi>59 || se>59) return false;
    if (s.size() > 19) {
      if (s[19]!='.' || s.size()<21 || s.size()>26) return false;
      int f; if (!number(20,s.size()-20,f)) return false;
      fraction = f; for (size_t i=s.size()-20; i<6; ++i) fraction *= 10;
    }
  }
  y -= m <= 2;
  const int era = y/400, yoe = y-era*400;
  const int doy = (153*(m+(m>2?-3:9))+2)/5+d-1;
  const int64_t day = int64_t(era)*146097 + yoe*365+yoe/4-yoe/100+doy-719468;
  micros = ((day*24+h)*3600+mi*60+se)*1000000+fraction;
  return true;
}
FileType infer(const std::string &s)
{
  int64_t i; double f; bool date;
  if (lower(s)=="true" || lower(s)=="false") return BOOL;
  if (integer(s,i)) return INT;
  if (temporal(s,i,date)) return date ? DATE : DATETIME;
  // Digit-only out-of-range integers remain text rather than losing precision.
  if (s.find_first_not_of("+-0123456789") == std::string::npos) return TEXT;
  if (real(s,f)) return REAL;
  return TEXT;
}
FileType widen(FileType a, FileType b)
{
  if (a==NIL) return b;
  if (b==NIL || a==b) return a;
  if (a>=BOOL && a<=REAL && b>=BOOL && b<=REAL) return std::max(a,b);
  if ((a==DATE || a==DATETIME) && (b==DATE || b==DATETIME)) return DATETIME;
  return TEXT;
}
int enabled(ObSQLSessionInfo &session)
{
  ObObj v; int ret = session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,v);
  if (OB_SUCC(ret) && !v.get_bool()) {
    ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled; set enable_file_sql=ON");
  }
  return ret;
}
int allowed(ObSQLSessionInfo &session, const std::string &path)
{
  int ret = enabled(session); ObString secure;
  if (OB_SUCC(ret)) ret = session.get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV,secure);
  if (OB_SUCC(ret)) {
    const std::string configured(secure.ptr() ? secure.ptr() : "",secure.length());
    char root[PATH_MAX]; struct stat s;
    if (configured.empty() || configured.find('\0')!=std::string::npos ||
        !realpath(configured.c_str(),root) || stat(root,&s)!=0 || !S_ISDIR(s.st_mode)) ret=OB_ERR_NO_PRIVILEGE;
    else {
      const size_t n=strlen(root);
      if (path.compare(0,n,root)!=0 || (n!=1 && path.size()>n && path[n]!='/')) ret=OB_ERR_NO_PRIVILEGE;
    }
    if (OB_FAIL(ret)) LOG_USER_ERROR(OB_INVALID_ARGUMENT,"file SQL path denied by secure_file_priv");
  }
  return ret;
}
bool same(const FileOptions &o, const struct stat &s)
{
  return o.device==uint64_t(s.st_dev) && o.inode==uint64_t(s.st_ino) && o.size==s.st_size &&
         o.sec==s.st_mtim.tv_sec && o.nsec==s.st_mtim.tv_nsec;
}
void fingerprint(FileOptions &o, const struct stat &s)
{ o.device=s.st_dev; o.inode=s.st_ino; o.size=s.st_size; o.sec=s.st_mtim.tv_sec; o.nsec=s.st_mtim.tv_nsec; }

// Native Parquet decoding: bounded footer metadata and one page per projected
// column. No query execution or full-file materialization is delegated.
struct CompactNode {
  int type=0; int64_t value=0; std::string bytes;
  std::vector<int> ids; std::vector<CompactNode> children;
  const CompactNode &field(int id) const {
    for (size_t i=0;i<ids.size();++i) if (ids[i]==id) return children[i];
    static const CompactNode empty; return empty;
  }
};
class CompactCursor {
public:
  explicit CompactCursor(const std::string &s) : s_(s) {}
  size_t position=0; bool valid=true;
  uint8_t byte() { if (position>=s_.size()) { valid=false; return 0; } return uint8_t(s_[position++]); }
  uint64_t varint() {
    uint64_t n=0;
    for (unsigned shift=0; valid && shift<64; shift+=7) {
      uint8_t b=byte(); if (shift==63 && (b&0xfe)) { valid=false; break; }
      n|=uint64_t(b&127)<<shift; if (!(b&128)) return n;
    }
    valid=false; return 0;
  }
  int64_t zigzag() { uint64_t v=varint(); return int64_t(v>>1)^-int64_t(v&1); }
  CompactNode parse(int type=12,unsigned depth=0) {
    CompactNode n; n.type=type;
    if (++nodes_>200000 || depth>32) { valid=false; return n; }
    switch(type) {
      case 1: n.value=1; break; case 2: n.value=0; break;
      case 3: n.value=int8_t(byte()); break;
      case 4: case 5: case 6: n.value=zigzag(); break;
      case 7:
        if (position+8>s_.size()) valid=false; else position+=8;
        break;
      case 8: {
        uint64_t length=varint();
        if (length>s_.size()-position) valid=false;
        else { n.bytes=s_.substr(position,length); position+=length; } break;
      }
      case 9: case 10: {
        uint8_t b=byte(); uint64_t count=b>>4; const int element=b&15;
        if (count==15) count=varint();
        if (count>200000) { valid=false; break; }
        for (uint64_t i=0;valid && i<count;++i) {
          if (element==1 || element==2) n.children.push_back(parse(byte(),depth+1));
          else n.children.push_back(parse(element,depth+1));
        } break;
      }
      case 11: {
        uint64_t count=varint(); if (count>100000) { valid=false; break; }
        if (count) {
          uint8_t types=byte();
          for (uint64_t i=0;valid && i<count;++i) {
            n.children.push_back(parse(types>>4,depth+1)); n.children.push_back(parse(types&15,depth+1));
          }
        } break;
      }
      case 12: {
        int id=0;
        while (valid) {
          uint8_t b=byte(); if (!b) break;
          int type=b&15; id=(b>>4)?id+(b>>4):int(zigzag());
          if (id<=0 || std::find(n.ids.begin(),n.ids.end(),id)!=n.ids.end()) { valid=false; break; }
          n.ids.push_back(id); n.children.push_back(parse(type,depth+1));
        } break;
      }
      default: valid=false; break;
    }
    return n;
  }
private:
  const std::string &s_; size_t nodes_=0;
};
uint64_t little(const std::string &s,size_t p,size_t width,bool &ok)
{
  uint64_t value=0;
  if (width>8 || p>s.size() || width>s.size()-p) { ok=false; return 0; }
  for (size_t i=0;i<width;++i) value|=uint64_t(uint8_t(s[p+i]))<<(8*i);
  return value;
}
bool snappy_decode(const std::string &input,std::string &out)
{
  CompactCursor c(input); const uint64_t expected=c.varint();
  if (!c.valid || expected>MAX_RECORD) return false;
  out.clear(); out.reserve(expected);
  while (c.valid && c.position<input.size()) {
    const uint8_t tag=c.byte(); const unsigned kind=tag&3;
    uint64_t length=0,offset=0;
    if (!kind) {
      length=tag>>2;
      if (length<60) ++length;
      else {
        const unsigned width=length-59;
        length=little(input,c.position,width,c.valid)+1; c.position+=width;
      }
      if (!c.valid || length>expected-out.size() || c.position>input.size() || length>input.size()-c.position) return false;
      out.append(input,c.position,length); c.position+=length;
    } else {
      if (kind==1) { length=4+((tag>>2)&7); offset=((tag&0xe0)<<3)|c.byte(); }
      else {
        length=1+(tag>>2); const unsigned width=kind==2?2:4;
        offset=little(input,c.position,width,c.valid); c.position+=width;
      }
      if (!c.valid || !offset || offset>out.size() || length>expected-out.size()) return false;
      for (uint64_t i=0;i<length;++i) out+=out[out.size()-offset];
    }
  }
  return c.valid && out.size()==expected;
}
bool hybrid_decode(const std::string &bytes,size_t &position,size_t end,unsigned bits,size_t count,std::vector<uint64_t> &values)
{
  if (bits>32 || end>bytes.size() || position>end || count>MAX_RECORD) return false;
  values.clear(); values.reserve(count);
  while (values.size()<count) {
    uint64_t header=0; unsigned shift=0;
    do {
      if (position>=end || shift>=64) return false;
      uint8_t b=uint8_t(bytes[position++]); if (shift==63 && (b&0xfe)) return false;
      header|=uint64_t(b&127)<<shift; shift+=7;
      if (!(b&128)) break;
    } while (true);
    if (header<2) return false;
    const uint64_t run=header>>1;
    if (header&1) {
      if (run>(count-values.size()+7)/8) return false;
      if (run>(end-position)/(bits?bits:1) && bits) return false;
      const uint64_t size=run*bits;
      if (size>end-position) return false;
      for (uint64_t i=0;i<run*8 && values.size()<count;++i) {
        uint64_t v=0;
        for (unsigned k=0;k<bits;++k) {
          const uint64_t bit=i*bits+k;
          v|=uint64_t((uint8_t(bytes[position+bit/8])>>(bit%8))&1)<<k;
        }
        values.push_back(v);
      }
      position+=size;
    } else {
      bool ok=true; const unsigned width=(bits+7)/8;
      const uint64_t v=little(bytes,position,width,ok); position+=width;
      if (!ok || position>end || (bits<32 && v>=(uint64_t(1)<<bits))) return false;
      if (run>count-values.size()) return false;
      values.insert(values.end(),run,v);
    }
  }
  return true;
}
struct ParquetSchema {
  int physical=0, converted=-1, fixed=0; bool optional=false;
  FileColumn column;
};
class ParquetReader {
  struct ColumnStream {
    size_t schema_index=0; int codec=0; int64_t offset=0,end=0;
    std::vector<FileValue> dictionary, values; size_t position=0; int64_t seen=0;
  };
public:
  int init(int fd,int64_t size) {
    fd_=fd; size_=size; schemas_.clear(); groups_.clear(); group_index_=0; row_index_=0; streams_.clear();
    std::string bytes;
    if (size<12 || !read(0,4,bytes) || bytes!="PAR1" || !read(size-8,8,bytes) || bytes.substr(4)!="PAR1") return fail("invalid Parquet magic");
    bool ok=true; const uint64_t length=little(bytes,0,4,ok);
    if (!ok || !length || length>MAX_RECORD || length>uint64_t(size-12) || !read(size-8-length,length,bytes)) return fail("invalid Parquet footer length");
    footer_start_=size-8-length; CompactCursor cursor(bytes); auto footer=cursor.parse();
    if (!cursor.valid || cursor.position!=bytes.size() || footer.field(2).type!=9 || footer.field(4).type!=9) return fail("malformed Parquet footer");
    const auto &schema=footer.field(2).children;
    if (schema.empty() || schema.size()>MAX_COLUMNS+1 || schema[0].field(5).value!=int64_t(schema.size()-1)) return fail("flat Parquet schema required");
    for (size_t i=1;i<schema.size();++i) {
      const auto &n=schema[i]; ParquetSchema s;
      if (n.field(1).type==0 || n.field(5).value!=0 || n.field(3).value>1 || n.field(4).bytes.empty()) return fail("nested Parquet schema is unsupported");
      s.physical=n.field(1).value; s.optional=n.field(3).value==1; s.fixed=n.field(2).value;
      if (n.field(6).type) s.converted=n.field(6).value;
      s.column.name=n.field(4).bytes; s.column.nullable=s.optional;
      switch(s.physical) {
        case 0:s.column.type=BOOL;break; case 1:case 2:s.column.type=INT;break;
        case 3:s.column.type=DATETIME;break; case 4:case 5:s.column.type=REAL;break;
        case 6:case 7:s.column.type=TEXT;break; default:return fail("unsupported Parquet physical type");
      }
      if (s.converted==6) s.column.type=DATE;
      else if (s.converted==9 || s.converted==10) s.column.type=DATETIME;
      else if (s.converted==14) s.column.type=TEXT;
      else if (s.converted==5) return fail("Parquet decimal is unsupported");
      const auto &logical=n.field(10);
      if (logical.field(6).type) s.column.type=DATE;
      if (logical.field(8).type) {
        s.column.type=DATETIME;
        const auto &unit=logical.field(8).field(2);
        if (unit.field(1).type) s.converted=9;
        else if (unit.field(2).type) s.converted=10;
        else return fail("nanosecond timestamps are unsupported");
      }
      schemas_.push_back(std::move(s));
    }
    groups_=footer.field(4).children;
    if (groups_.size()>100000) return fail("too many Parquet row groups");
    int64_t total=0;
    for (auto &g:groups_) {
      const int64_t rows=g.field(3).value;
      if (rows<0 || rows>INT64_MAX-total || g.field(1).children.size()!=schemas_.size()) return fail("invalid Parquet row group");
      total+=rows;
      for (auto &chunk:g.field(1).children) if (!chunk.field(1).bytes.empty()) return fail("external Parquet column files are forbidden");
    }
    if (total!=footer.field(3).value) return fail("Parquet row count mismatch");
    return OB_SUCCESS;
  }
  const std::vector<ParquetSchema> &schema() const { return schemas_; }
  int next(const ObIArray<uint64_t> &projection,std::vector<FileValue> &out) {
    for (;;) {
      if (group_index_>=groups_.size()) return OB_ITER_END;
      const auto &group=groups_[group_index_];
      if (row_index_>=group.field(3).value) {
        for (auto &stream:streams_) if (stream.position!=stream.values.size() || stream.seen!=row_index_) return fail("Parquet column row count mismatch");
        ++group_index_; row_index_=0; streams_.clear(); continue;
      }
      if (streams_.empty() && projection.count()) {
        for (int64_t i=0;i<projection.count();++i) {
          ColumnStream stream; stream.schema_index=projection.at(i)-OB_APP_MIN_COLUMN_ID;
          if (stream.schema_index>=schemas_.size()) return OB_ERR_UNEXPECTED;
          const auto &meta=group.field(1).children[stream.schema_index].field(3);
          const auto &s=schemas_[stream.schema_index];
          if (meta.type!=12 || meta.field(1).value!=s.physical || meta.field(5).value!=group.field(3).value ||
              meta.field(3).children.size()!=1 || meta.field(3).children[0].bytes!=s.column.name) return fail("Parquet column metadata mismatch");
          stream.codec=meta.field(4).value; stream.offset=meta.field(9).value;
          if (meta.field(11).type && meta.field(11).value<stream.offset) stream.offset=meta.field(11).value;
          const int64_t compressed=meta.field(7).value;
          if (compressed<0 || stream.offset<4 || stream.offset>footer_start_ || compressed>footer_start_-stream.offset) return fail("invalid Parquet chunk offset");
          stream.end=stream.offset+compressed; streams_.push_back(std::move(stream));
        }
      }
      out.clear();
      for (auto &stream:streams_) {
        while (stream.position>=stream.values.size()) {
          int ret=page(stream); if (OB_FAIL(ret)) return ret;
        }
        out.push_back(stream.values[stream.position++]);
      }
      ++row_index_; return OB_SUCCESS;
    }
  }
private:
  int fail(const char *s) { return error(s); }
  bool read(int64_t offset,size_t length,std::string &out) {
    if (length>MAX_RECORD || offset<0 || offset>size_ || uint64_t(size_-offset)<length) return false;
    out.resize(length); size_t done=0;
    while (done<length) {
      const ssize_t n=pread(fd_,&out[done],length-done,offset+done);
      if (n<0 && errno==EINTR) continue;
      if (n<=0) return false;
      done+=n;
    }
    return true;
  }
  bool plain(const std::string &bytes,size_t &p,size_t count,const ParquetSchema &s,std::vector<FileValue> &values) {
    if (count>262144) return false;
    bool ok=true; values.clear(); values.reserve(count);
    for (size_t i=0;ok && i<count;++i) {
      FileValue v; v.type=s.column.type; uint64_t u=0;
      if (s.physical==0) {
        if (p+i/8>=bytes.size()) return false;
        v.text=(uint8_t(bytes[p+i/8])&(1<<(i%8)))?"true":"false";
      } else if (s.physical==6 || s.physical==7) {
        uint64_t length=s.fixed;
        if (s.physical==6) { length=little(bytes,p,4,ok); p+=4; }
        if (!ok || p>bytes.size() || length>bytes.size()-p) return false;
        v.text=bytes.substr(p,length); p+=length;
      } else {
        const size_t width=s.physical==1 || s.physical==4?4:8;
        u=little(bytes,p,width,ok); p+=width; if (!ok) return false;
        int64_t number=s.physical==1?int64_t(int32_t(u)):int64_t(u);
        if (s.physical==3) {
          const int64_t day=int64_t(little(bytes,p,4,ok))-2440588; p+=4;
          if (!ok || u>=86400000000000ULL || day<-719162 || day>2932896) return false;
          v.temporal_encoded=true; v.temporal_value=day*86400000000LL+u/1000;
        } else if (s.column.type==DATE || s.column.type==DATETIME) {
          v.temporal_encoded=true;
          if (s.column.type==DATE) {
            if (number<-719162 || number>2932896) return false;
            v.temporal_value=number;
          }
          else {
            if (s.converted==9 && (number>INT64_MAX/1000 || number<INT64_MIN/1000)) return false;
            v.temporal_value=s.converted==9?number*1000:number;
            if (v.temporal_value<-62135596800000000LL || v.temporal_value>253402300799999999LL) return false;
          }
        } else if (s.physical==4 || s.physical==5) {
          double d;
          if (s.physical==4) { uint32_t bits=u; float f; memcpy(&f,&bits,4); d=f; }
          else memcpy(&d,&u,8);
          if (!std::isfinite(d)) return false;
          std::ostringstream text; text<<std::setprecision(17)<<d; v.text=text.str();
        } else v.text=s.converted==14?std::to_string(u):std::to_string(number);
      }
      values.push_back(std::move(v));
    }
    if (s.physical==0) p+=(count+7)/8;
    return ok;
  }
  int page(ColumnStream &stream) {
    stream.values.clear(); stream.position=0;
    if (stream.offset>=stream.end) return fail("truncated Parquet column");
    std::string bytes;
    const size_t header_size=std::min<int64_t>(65536,stream.end-stream.offset);
    if (!read(stream.offset,header_size,bytes)) return fail("Parquet page header read failed");
    CompactCursor cursor(bytes); const auto header=cursor.parse();
    if (!cursor.valid) return fail("invalid Parquet page header");
    const int64_t uncompressed=header.field(2).value,compressed=header.field(3).value;
    stream.offset+=cursor.position;
    if (uncompressed<0 || uncompressed>int64_t(MAX_RECORD) || compressed<0 || compressed>int64_t(MAX_RECORD) || compressed>stream.end-stream.offset || !read(stream.offset,compressed,bytes)) return fail("invalid Parquet page size");
    stream.offset+=compressed;
    const int kind=header.field(1).value; const bool v2=kind==3;
    const auto &data=header.field(v2?8:5);
    size_t prefix=0;
    if (v2) {
      const int64_t def=data.field(5).value,rep=data.field(6).value;
      if (def<0 || rep!=0 || def>compressed) return fail("invalid Parquet V2 levels");
      prefix=def;
    }
    const bool decompress=stream.codec!=0 && (!v2 || data.field(7).type==0 || data.field(7).value!=0);
    if (decompress) {
      if (stream.codec!=1) return fail("unsupported Parquet compression codec");
      std::string output;
      if (!snappy_decode(bytes.substr(prefix),output)) return fail("invalid Snappy page");
      bytes=bytes.substr(0,prefix)+output;
    }
    if (bytes.size()!=uint64_t(uncompressed)) return fail("Parquet page length mismatch");
    const auto &schema=schemas_[stream.schema_index]; size_t p=0;
    if (kind==2) {
      const auto &dict=header.field(7); const int64_t count=dict.field(1).value;
      if (count<0 || dict.field(2).value!=0 || !stream.dictionary.empty() || !plain(bytes,p,count,schema,stream.dictionary) || p!=bytes.size()) return fail("invalid Parquet dictionary");
      return OB_SUCCESS;
    }
    if (kind==1) return OB_SUCCESS;
    if (kind!=0 && kind!=3) return fail("unsupported Parquet page type");
    const int64_t count=data.field(1).value;
    const int64_t remaining=groups_[group_index_].field(3).value-stream.seen;
    if (count<=0 || count>remaining || count>262144) return fail("invalid Parquet value count");
    std::vector<uint64_t> levels;
    if (schema.optional) {
      bool ok=true; size_t length=prefix;
      if (!v2) {
        if (data.field(3).value!=3) return fail("Parquet definition levels require RLE");
        length=little(bytes,0,4,ok); p=4;
      }
      if (!ok || length>bytes.size()-p || !hybrid_decode(bytes,p,p+length,1,count,levels)) return fail("invalid Parquet definition levels");
      p=(v2?0:4)+length;
    } else if (v2 && prefix!=0) return fail("unexpected definition levels");
    size_t nonnull=0;
    for (int64_t i=0;i<count;++i) if (!schema.optional || levels[i]==1) ++nonnull;
    std::vector<FileValue> decoded; const int encoding=data.field(v2?4:2).value;
    if (nonnull==0) {
      // Writers may retain the dictionary bit-width byte on an all-NULL page.
      if ((encoding==2 || encoding==8) && p<bytes.size()) {
        if (uint8_t(bytes[p++])>32) return fail("invalid empty Parquet dictionary width");
      }
    } else if (encoding==0) {
      if (!plain(bytes,p,nonnull,schema,decoded)) return fail("invalid Parquet plain values");
    } else if (encoding==3 && schema.physical==0) {
      bool ok=true; const uint64_t length=little(bytes,p,4,ok); p+=4;
      std::vector<uint64_t> booleans;
      if (!ok || p>bytes.size() || length!=bytes.size()-p ||
          !hybrid_decode(bytes,p,bytes.size(),1,nonnull,booleans)) return fail("invalid Parquet boolean RLE");
      for (auto boolean:booleans) {
        FileValue value; value.type=BOOL; value.text=boolean?"true":"false";
        decoded.push_back(std::move(value));
      }
    } else if (encoding==2 || encoding==8) {
      if (p>=bytes.size() || stream.dictionary.empty()) return fail("missing Parquet dictionary");
      unsigned bits=uint8_t(bytes[p++]); std::vector<uint64_t> indices;
      if (!hybrid_decode(bytes,p,bytes.size(),bits,nonnull,indices)) return fail("invalid Parquet dictionary indices");
      for (auto index:indices) {
        if (index>=stream.dictionary.size()) return fail("Parquet dictionary index out of range");
        decoded.push_back(stream.dictionary[index]);
      }
    } else return fail("unsupported Parquet value encoding");
    if (p!=bytes.size()) return fail("trailing Parquet page bytes");
    size_t index=0;
    for (int64_t i=0;i<count;++i) {
      if (schema.optional && levels[i]==0) { FileValue value; value.null=true; stream.values.push_back(std::move(value)); }
      else stream.values.push_back(std::move(decoded[index++]));
    }
    stream.seen+=count;
    size_t memory=0;
    for (const auto &s:streams_) {
      memory+=(s.values.capacity()+s.dictionary.capacity())*sizeof(FileValue);
      for (const auto &v:s.values) memory+=v.text.capacity();
      for (const auto &v:s.dictionary) memory+=v.text.capacity();
    }
    if (memory>128*1024*1024) return fail("Parquet scan exceeds 128 MiB page memory budget");
    // A projected column retains at most one bounded page and its dictionary.
    return OB_SUCCESS;
  }
  int fd_=-1; int64_t size_=0,footer_start_=0,row_index_=0; size_t group_index_=0;
  std::vector<ParquetSchema> schemas_; std::vector<CompactNode> groups_; std::vector<ColumnStream> streams_;
};

class FileReader {
public:
  ~FileReader() { if (file_) fclose(file_); }
  int open(FileOptions &o, ObSQLSessionInfo &session, bool bind)
  {
    if (file_) { fclose(file_); file_=nullptr; }
    options_=&o; session_=&session; line_=0;
    char canonical[PATH_MAX];
    if (o.path.find('\0')!=std::string::npos || !realpath(o.path.c_str(),canonical)) return error("cannot resolve file",o.path);
    int ret = allowed(session,canonical); if (OB_FAIL(ret)) return ret;
    const int fd = ::open(canonical,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return error("cannot open file",o.path);
    struct stat s;
    if (fstat(fd,&s)!=0 || !S_ISREG(s.st_mode)) { ::close(fd); return error("regular file required",o.path); }
    // Check the opened object, not just the pathname, against the boundary.
    const std::string fdpath="/proc/self/fd/"+std::to_string(fd);
    if (!realpath(fdpath.c_str(),canonical) || OB_FAIL(allowed(session,canonical))) {
      ::close(fd); return OB_SUCC(ret) ? OB_ERR_NO_PRIVILEGE : ret;
    }
    if (!bind && !same(o,s)) { ::close(fd); return error("file changed after schema inference",o.path); }
    if (bind) { o.path=canonical; fingerprint(o,s); }
    file_=fdopen(fd,"rb");
    if (!file_) { ::close(fd); return error("cannot open stream",o.path); }
    return OB_SUCCESS;
  }
  int unchanged()
  {
    struct stat s, path;
    if (!file_ || fstat(fileno(file_),&s)!=0 || stat(options_->path.c_str(),&path)!=0 ||
        !same(*options_,s) || !same(*options_,path)) return error("file changed during scan",options_->path,line_);
    return OB_SUCCESS;
  }
  int next(std::vector<FileValue> &values)
  {
    int interrupted=OB_SUCCESS;
    if (session_ && session_->is_terminate(interrupted)) return interrupted;
    int ret=unchanged(); if (OB_FAIL(ret)) return ret;
    return options_->format=="csv" ? csv(values) : json(values);
  }
  int64_t line() const { return line_; }
  int descriptor() const { return file_?fileno(file_):-1; }
private:
  int csv(std::vector<FileValue> &values)
  {
    for (;;) {
      values.clear(); FileValue v; bool in_quote=false, after_quote=false, started=false;
      size_t size=0; int c;
      while ((c=fgetc(file_))!=EOF) {
        started=true;
        if (++size>MAX_RECORD) return error("record exceeds 16 MiB",options_->path,line_+1);
        if (in_quote) {
          if (c==options_->escape && options_->escape!=options_->quote) {
            c=fgetc(file_); if (c==EOF) return error("incomplete escape",options_->path,line_+1);
            v.text+=char(c);
          } else if (c==options_->quote) {
            int peek=fgetc(file_);
            if (peek==options_->quote && options_->escape==options_->quote) v.text+=char(c);
            else { if (peek!=EOF) ungetc(peek,file_); in_quote=false; after_quote=true; }
          } else if (c=='\r') {
            int peek=fgetc(file_);
            if (peek=='\n') v.text+='\n';
            else { v.text+='\r'; if (peek!=EOF) ungetc(peek,file_); }
          } else v.text+=char(c);
        } else if (c==options_->delimiter) {
          values.push_back(std::move(v)); v=FileValue(); after_quote=false;
          if (values.size()>=MAX_COLUMNS) return error("too many columns",options_->path,line_+1);
        } else if (c=='\n' || c=='\r') {
          if (c=='\r') { int peek=fgetc(file_); if (peek!=EOF && peek!='\n') ungetc(peek,file_); }
          break;
        } else if (after_quote) return error("characters after closing quote",options_->path,line_+1);
        else if (c==options_->quote && v.text.empty()) { in_quote=true; v.quoted=true; }
        else if (c==options_->quote) return error("quote in unquoted field",options_->path,line_+1);
        else v.text+=char(c);
      }
      if (ferror(file_)) return error("read failed",options_->path,line_+1);
      if (!started && values.empty() && v.text.empty()) return OB_ITER_END;
      ++line_;
      if (in_quote) return error("unterminated quoted field",options_->path,line_);
      values.push_back(std::move(v));
      if (values.size()==1 && values[0].text.empty() && !values[0].quoted && options_->blank) continue;
      for (auto &value : values) {
        int64_t valid_length=0;
        if (ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN,value.text.data(),value.text.size(),valid_length)!=OB_SUCCESS ||
            valid_length!=int64_t(value.text.size())) return error("invalid UTF-8 CSV field",options_->path,line_);
        value.null=!value.quoted && value.text==options_->nullstr;
        value.type=value.null ? NIL : infer(value.text);
      }
      return OB_SUCCESS;
    }
  }
  int json(std::vector<FileValue> &values)
  {
    for (;;) {
      std::string bytes; int c;
      while ((c=fgetc(file_))!=EOF && c!='\n') {
        if (bytes.size()>=MAX_RECORD) return error("record exceeds 16 MiB",options_->path,line_+1);
        bytes+=char(c);
      }
      if (ferror(file_)) return error("read failed",options_->path,line_+1);
      if (bytes.empty() && c==EOF) return OB_ITER_END;
      ++line_; if (!bytes.empty() && bytes.back()=='\r') bytes.pop_back();
      if (bytes.find_first_not_of(" \t\r")==std::string::npos && options_->blank) continue;
      json_allocator_.reuse(); ObIJsonBase *base=nullptr;
      int ret=ObJsonBaseFactory::get_json_base(&json_allocator_,str(bytes),ObJsonInType::JSON_TREE,ObJsonInType::JSON_TREE,base);
      if (OB_FAIL(ret) || !base || base->json_type()!=ObJsonNodeType::J_OBJECT) return error("JSONL object required",options_->path,line_);
      auto *object=static_cast<ObJsonObject *>(base);
      if (object->element_count()>MAX_COLUMNS) return error("too many JSON fields",options_->path,line_);
      values.clear(); names_.clear();
      for (uint64_t i=0; i<object->element_count(); ++i) {
        ObString key; ret=object->get_key(i,key); if (OB_FAIL(ret)) return ret;
        names_.emplace_back(key.ptr(),key.length()); FileValue v;
        ObJsonNode *node=object->get_value(i); if (!node) return OB_ERR_UNEXPECTED;
        switch (node->json_type()) {
          case ObJsonNodeType::J_NULL: v.null=true; break;
          case ObJsonNodeType::J_STRING: {
            ObString value=static_cast<ObJsonString *>(node)->get_str(); v.text.assign(value.ptr(),value.length());
            int64_t us; bool date; v.type=temporal(v.text,us,date) ? (date?DATE:DATETIME) : TEXT; break;
          }
          case ObJsonNodeType::J_BOOLEAN: v.type=BOOL; v.text=node->get_boolean()?"true":"false"; break;
          case ObJsonNodeType::J_OBJECT: case ObJsonNodeType::J_ARRAY:
            return error("nested JSON values are not supported",options_->path,line_);
          default: {
            ObJsonBuffer buffer(&json_allocator_); ret=node->print(buffer,0); if (OB_FAIL(ret)) return ret;
            ObString value=buffer.string(); v.text.assign(value.ptr(),value.length()); v.type=infer(v.text);
          }
        }
        values.push_back(std::move(v));
      }
      return OB_SUCCESS;
    }
  }
public:
  const std::vector<std::string> &names() const { return names_; }
private:
  FILE *file_=nullptr;
  FileOptions *options_=nullptr;
  ObSQLSessionInfo *session_=nullptr;
  int64_t line_=0;
  ObArenaAllocator json_allocator_{"FileJsonRow"};
  std::vector<std::string> names_;
};

std::string encode(const FileOptions &o)
{
  std::ostringstream s;
  s<<std::quoted(o.path)<<' '<<std::quoted(o.format)<<' '<<std::quoted(o.nullstr)<<' '
   <<o.header<<' '<<o.blank<<' '<<o.delimiter<<' '<<o.quote<<' '<<o.escape<<' '<<o.skip<<' '
   <<o.device<<' '<<o.inode<<' '<<o.size<<' '<<o.sec<<' '<<o.nsec<<' '<<o.columns.size()<<' ';
  for (const auto &c:o.columns) s<<std::quoted(c.name)<<' '<<int(c.type)<<' '<<c.nullable<<' '<<c.scale<<' ';
  return s.str();
}
int decode(const ObString &bytes, FileOptions &o)
{
  std::istringstream s(std::string(bytes.ptr(),bytes.length())); size_t n;
  s>>std::quoted(o.path)>>std::quoted(o.format)>>std::quoted(o.nullstr)>>o.header>>o.blank
   >>o.delimiter>>o.quote>>o.escape>>o.skip>>o.device>>o.inode>>o.size>>o.sec>>o.nsec>>n;
  if (!s || n>MAX_COLUMNS) return OB_INVALID_ARGUMENT;
  o.columns.clear();
  for (size_t i=0;i<n;++i) { FileColumn c; int type; s>>std::quoted(c.name)>>type>>c.nullable>>c.scale;
    if (!s || type<NIL || type>DATETIME || c.scale<0 || c.scale>6) return OB_INVALID_ARGUMENT;
    c.type=FileType(type); o.columns.push_back(std::move(c)); }
  return OB_SUCCESS;
}
int infer_schema(FileOptions &o, ObSQLSessionInfo &session)
{
  FileReader reader; int ret=reader.open(o,session,true); if (OB_FAIL(ret)) return ret;
  if (o.format=="parquet") {
    ParquetReader parquet; ret=parquet.init(reader.descriptor(),o.size);
    if (OB_SUCC(ret)) for (const auto &s:parquet.schema()) {
      FileColumn c=s.column; if (c.type==DATETIME) c.scale=6; o.columns.push_back(std::move(c));
    }
    return OB_SUCC(ret)?reader.unchanged():ret;
  }
  std::vector<FileValue> values; int64_t rows=0;
  if (o.format=="csv") {
    for (int64_t i=0; i<o.skip; ++i) { ret=reader.next(values); if (OB_FAIL(ret)) return error("skip exceeds file",o.path); }
    ret=reader.next(values);
    if (ret==OB_ITER_END) return error("cannot infer an empty CSV file",o.path);
    if (OB_FAIL(ret)) return ret;
    for (size_t i=0;i<values.size();++i) {
      FileColumn c; c.name=o.header?values[i].text:"column"+std::to_string(i);
      if (i==0 && c.name.compare(0,3,"\xef\xbb\xbf")==0) c.name.erase(0,3);
      if (c.name.empty()) c.name="column"+std::to_string(i);
      const std::string original=c.name; int suffix=2;
      auto duplicate=[&]() { for (auto &old:o.columns) if (lower(old.name)==lower(c.name)) return true; return false; };
      while (duplicate()) c.name=original+"__"+std::to_string(suffix++);
      o.columns.push_back(std::move(c));
    }
    if (o.header) ret=reader.next(values);
  } else ret=reader.next(values);
  for (; ret==OB_SUCCESS; ret=reader.next(values)) {
    if (o.format=="csv" && values.size()!=o.columns.size()) return error("CSV field count mismatch",o.path,reader.line());
    if (o.format!="csv") {
      for (auto &name:reader.names()) {
        bool found=false; for (auto &c:o.columns) if (c.name==name) { found=true; break; }
        if (!found) {
          if (o.columns.size()>=MAX_COLUMNS) return error("too many union columns",o.path,reader.line());
          FileColumn c; c.name=name; c.nullable=rows>0; o.columns.push_back(std::move(c));
        }
      }
      for (auto &c:o.columns) {
        if (std::find(reader.names().begin(),reader.names().end(),c.name)==reader.names().end()) c.nullable=true;
      }
    }
    for (size_t i=0;i<values.size();++i) {
      size_t column=i;
      if (o.format!="csv") { for (column=0;column<o.columns.size() && o.columns[column].name!=reader.names()[i];++column) {} }
      auto &c=o.columns[column]; c.type=widen(c.type,values[i].type); c.nullable |= values[i].null;
      if (values[i].type==DATETIME && values[i].text.size()>19) {
        c.scale=std::max(c.scale,int(values[i].text.size()-20));
      }
    }
    ++rows;
    if ((rows&1023)==0 && OB_FAIL(reader.unchanged())) return ret;
  }
  if (ret!=OB_ITER_END) return ret;
  for (auto &c:o.columns) if (c.type==NIL) { c.type=TEXT; c.nullable=true; }
  if (o.columns.empty()) return error("cannot infer empty schema",o.path);
  return reader.unchanged();
}
int scalar(const FileValue &v, FileType type, ObObj &obj)
{
  if (v.null) { obj.set_null(); return OB_SUCCESS; }
  int64_t i; double f; bool date;
  switch (type) {
    case TEXT: obj.set_varchar(str(v.text)); obj.set_collation_type(CS_TYPE_UTF8MB4_BIN); break;
    case BOOL: obj.set_tinyint(lower(v.text)=="true"); break;
    case INT:
      if (v.type==BOOL) i=lower(v.text)=="true";
      else if (!integer(v.text,i)) return OB_INVALID_ARGUMENT;
      obj.set_int(i); break;
    case REAL:
      if (v.type==BOOL) f=lower(v.text)=="true";
      else if (!real(v.text,f)) return OB_INVALID_ARGUMENT;
      obj.set_double(f); break;
    case DATE: case DATETIME:
      if (v.temporal_encoded) {
        if (type==DATE) obj.set_date(v.temporal_value); else obj.set_datetime(v.temporal_value);
        break;
      }
      if (!temporal(v.text,i,date)) return OB_INVALID_ARGUMENT;
      if (type==DATE) obj.set_date(i/86400000000LL); else obj.set_datetime(i); break;
    default: obj.set_null(); break;
  }
  return OB_SUCCESS;
}

int glob_pattern(FileOptions &o, ObSQLSessionInfo &session)
{
  if (o.path.empty() || o.path.find('\0')!=std::string::npos) return error("invalid glob pattern");
  const size_t wildcard=o.path.find_first_of("*?[");
  const size_t slash=o.path.rfind('/',wildcard==std::string::npos?o.path.size():wildcard);
  const std::string parent=slash==std::string::npos?".":o.path.substr(0,slash+1);
  char canonical[PATH_MAX];
  if (!realpath(parent.c_str(),canonical)) return error("cannot resolve glob directory",parent);
  int ret=allowed(session,canonical);
  if (OB_SUCC(ret)) o.path=std::string(canonical)+(strcmp(canonical,"/")==0?"":"/")+o.path.substr(slash==std::string::npos?0:slash+1);
  return ret;
}
class GlobTableScan final : public ITableFunctionScan {
public:
  explicit GlobTableScan(const TableFunctionScanContext &c) : context_(c.exec_ctx_), args_(c.arguments_), projection_(c.column_ids_) {}
  int open() override { return rescan(args_); }
  int rescan(const ObIArray<ObObj> &args) override
  {
    files_.clear(); position_=0;
    FileOptions o;
    if (args.count()!=1 || !args.at(0).is_string_type()) return OB_INVALID_ARGUMENT;
    int ret=decode(args.at(0).get_string(),o); if (OB_FAIL(ret)) return ret;
    ret=glob_pattern(o,*context_.get_my_session()); if (OB_FAIL(ret)) return ret;
    try {
      const size_t wildcard=o.path.find_first_of("*?[");
      const size_t slash=o.path.rfind('/',wildcard==std::string::npos?o.path.size():wildcard);
      const std::string base=o.path.substr(0,slash+1);
      std::vector<std::string> parts;
      size_t from=slash+1;
      for (size_t next; from<o.path.size(); from=next+1) {
        next=o.path.find('/',from); if (next==std::string::npos) next=o.path.size();
        if (next>from) parts.push_back(o.path.substr(from,next-from));
      }
      if (parts.empty()) return OB_SUCCESS;
      if (parts.size()>64) return error("glob exceeds 64 path components",o.path);
      size_t visited=0;
      std::function<int(const std::string &,size_t)> discover;
      discover=[&](const std::string &directory,size_t depth) -> int {
        char canonical[PATH_MAX];
        if (!realpath(directory.c_str(),canonical)) return error("glob directory changed",directory);
        int status=allowed(*context_.get_my_session(),canonical); if (OB_FAIL(status)) return status;
        std::unique_ptr<DIR,int(*)(DIR*)> entries(opendir(canonical),closedir);
        if (!entries) return error("cannot open glob directory",directory);
        const std::string proc="/proc/self/fd/"+std::to_string(dirfd(entries.get()));
        if (!realpath(proc.c_str(),canonical)) return error("glob directory changed",directory);
        status=allowed(*context_.get_my_session(),canonical); if (OB_FAIL(status)) return status;
        for (;;) {
          status=context_.check_status(); if (OB_FAIL(status)) return status;
          errno=0; dirent *entry=readdir(entries.get());
          if (!entry) return errno==0?OB_SUCCESS:error("cannot read glob directory",directory);
          if (++visited>1000000) return error("glob exceeds one million directory entries",o.path);
          if (fnmatch(parts[depth].c_str(),entry->d_name,FNM_PERIOD)!=0) continue;
          const std::string candidate=directory+"/"+entry->d_name;
          if (candidate.size()>=PATH_MAX) return error("glob path exceeds resource limit",candidate);
          struct stat st;
          if (stat(candidate.c_str(),&st)!=0) {
            if (errno==ENOENT) continue; // A dangling symlink is not a file.
            return error("glob file changed",candidate);
          }
          if (depth+1<parts.size()) {
            if (S_ISDIR(st.st_mode)) {
              status=discover(candidate,depth+1); if (OB_FAIL(status)) return status;
            }
          } else if (S_ISREG(st.st_mode)) {
            if (!realpath(candidate.c_str(),canonical)) return error("glob file changed",candidate);
            status=allowed(*context_.get_my_session(),canonical); if (OB_FAIL(status)) return status;
            if (files_.size()>=10000) return error("glob exceeds 10000 files",o.path);
            files_.emplace_back(canonical);
          }
        }
      };
      ret=discover(base,0);
      if (OB_SUCC(ret)) std::sort(files_.begin(),files_.end());
      return ret;
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
  }

  int get_next_row(ObNewRow *&row) override
  {
    if (position_>=files_.size()) return OB_ITER_END;
    const auto &file=files_[position_++];
    int ret=allowed(*context_.get_my_session(),file); if (OB_FAIL(ret)) return ret;
    value_.set_varchar(str(file)); value_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    row_.cells_=&value_; row_.count_=projection_.count(); row=&row_; return OB_SUCCESS;
  }
  int close() override { files_.clear(); return OB_SUCCESS; }
private:
  ObExecContext &context_; const ObIArray<ObObj> &args_; const ObIArray<uint64_t> &projection_;
  std::vector<std::string> files_; size_t position_=0; ObObj value_; ObNewRow row_;
};

class FileTableScan final : public ITableFunctionScan {
public:
  explicit FileTableScan(const TableFunctionScanContext &ctx) : context_(ctx.exec_ctx_), projection_(ctx.column_ids_), args_(ctx.arguments_) {}
  int open() override { return rescan(args_); }
  int rescan(const ObIArray<ObObj> &args) override
  {
    try {
      reader_.reset(); values_.clear(); cells_.clear();
      if (args.count()!=1 || !args.at(0).is_string_type()) return OB_INVALID_ARGUMENT;
      int ret=decode(args.at(0).get_string(),options_); if (OB_FAIL(ret)) return ret;
      cells_.resize(projection_.count()); reader_.reset(new FileReader());
      ret=reader_->open(options_,*context_.get_my_session(),false); if (OB_FAIL(ret)) return ret;
      if (options_.format=="parquet") return parquet_.init(reader_->descriptor(),options_.size);
      if (options_.format=="csv") {
        for (int64_t i=0; i<options_.skip+(options_.header?1:0); ++i) {
          ret=reader_->next(values_); if (OB_FAIL(ret)) return ret;
        }
      }
      return OB_SUCCESS;
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
  }
  int get_next_row(ObNewRow *&row) override
  {
    try {
      int ret=context_.check_status(); if (OB_FAIL(ret)) return ret;
      if (options_.format=="parquet") {
        ret=reader_->unchanged(); if (OB_SUCC(ret)) ret=parquet_.next(projection_,values_);
      } else ret=reader_->next(values_);
      if (OB_FAIL(ret)) return ret;
      if (options_.format=="csv" && values_.size()!=options_.columns.size()) return error("CSV field count mismatch",options_.path,reader_->line());
      for (size_t i=0;i<cells_.size();++i) {
        const uint64_t column=projection_.at(i)-OB_APP_MIN_COLUMN_ID;
        if (column>=options_.columns.size()) return OB_ERR_UNEXPECTED;
        size_t index=options_.format=="parquet"?i:column;
        if (options_.format=="json") {
          const auto &names=reader_->names();
          for (index=0;index<names.size() && names[index]!=options_.columns[column].name;++index) {}
        }
        if (index>=values_.size()) cells_[i].set_null();
        else if (OB_FAIL(scalar(values_[index],options_.columns[column].type,cells_[i]))) return error("value incompatible with schema",options_.path,reader_->line());
      }
      row_.cells_=cells_.data(); row_.count_=cells_.size(); row=&row_; return OB_SUCCESS;
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
  }
  int close() override { reader_.reset(); values_.clear(); cells_.clear(); return OB_SUCCESS; }
private:
  ObExecContext &context_; const ObIArray<uint64_t> &projection_; const ObIArray<ObObj> &args_;
  FileOptions options_; std::unique_ptr<FileReader> reader_;
  ParquetReader parquet_;
  std::vector<FileValue> values_; std::vector<ObObj> cells_; ObNewRow row_;
};
std::string node_string(const ParseNode *n)
{ return n && n->str_value_ ? std::string(n->str_value_,n->str_len_) : std::string(); }
int options(const TableFunctionCall &call, FileOptions &o)
{
  o.format=call.name_.case_compare("read_csv")==0?"csv":"json";
  if (call.name_.case_compare("glob")==0) o.format="glob";
  if (call.name_.case_compare("read_parquet")==0) o.format="parquet";
  if (!call.arguments_ || call.arguments_->num_child_<1) return OB_INVALID_ARGUMENT;
  const ParseNode *path=call.arguments_->children_[0];
  if (path->type_!=T_VARCHAR) return error("file path must be a constant string");
  o.path=node_string(path); std::vector<std::string> seen;
  for (int64_t i=1;i<call.arguments_->num_child_;++i) {
    const ParseNode *arg=call.arguments_->children_[i];
    if (!arg || arg->type_!=T_OP_EQ || arg->num_child_!=2) return OB_ERR_PARSE_SQL;
    const ParseNode *key=arg->children_[0], *value=arg->children_[1];
    if (key->type_==T_COLUMN_REF && key->num_child_==3) key=key->children_[2];
    std::string name=lower(node_string(key));
    if (std::find(seen.begin(),seen.end(),name)!=seen.end()) return error("duplicate file option");
    seen.push_back(name); std::string text=node_string(value);
    if (name=="header" || name=="skip_blank_lines") {
      if (value->type_!=T_BOOL && value->type_!=T_INT) return error("boolean file option required");
      if (value->value_!=0 && value->value_!=1) return error("boolean file option required");
      (name=="header"?o.header:o.blank)=value->value_!=0;
    } else if (name=="sample_size") {
      bool all=value->type_==T_INT && value->value_==-1;
      all |= value->type_==T_OP_NEG && value->num_child_==1 && value->children_[0]->type_==T_INT && value->children_[0]->value_==1;
      if (!all) { LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL sample_size other than -1"); return OB_NOT_SUPPORTED; }
    } else if (name=="skip" || name=="skip_rows") {
      if (value->type_!=T_INT || value->value_<0) return error("skip must be a nonnegative integer");
      o.skip=value->value_;
    } else if (name=="format") {
      if (o.format!="json" || (text!="newline_delimited" && text!="ndjson" && text!="jsonl")) return error("newline_delimited JSON required");
    } else if (name=="nullstr" || name=="null") {
      if (value->type_!=T_VARCHAR) return error("nullstr must be text"); o.nullstr=text;
    } else if (name=="delim" || name=="delimiter" || name=="sep" || name=="quote" || name=="escape") {
      if (value->type_!=T_VARCHAR || text.size()!=1 || text[0]=='\0' || text[0]=='\r' || text[0]=='\n') return error("single-byte CSV option required");
      if (name=="quote") o.quote=uint8_t(text[0]); else if (name=="escape") o.escape=uint8_t(text[0]); else o.delimiter=uint8_t(text[0]);
    } else return error("unknown file option");
  }
  if (o.delimiter==o.quote) return error("delimiter and quote must differ");
  return OB_SUCCESS;
}
} // anonymous namespace

int create_file_table_scan(const TableFunctionScanContext &ctx, ITableFunctionScan *&scan)
{
  if (ctx.descriptor_.id_==13) {
    void *memory=ctx.allocator_.alloc(sizeof(GlobTableScan));
    if (!memory) return OB_ALLOCATE_MEMORY_FAILED;
    scan=new (memory) GlobTableScan(ctx); return OB_SUCCESS;
  }
  void *memory=ctx.allocator_.alloc(sizeof(FileTableScan));
  if (!memory) return OB_ALLOCATE_MEMORY_FAILED;
  scan=new (memory) FileTableScan(ctx); return OB_SUCCESS;
}
int ObDMLResolver::bind_file_table_function(const TableFunctionCall &call, TableItem *&item)
{
  // Output columns and file identity are resolved afresh for each statement.
  if (params_.query_ctx_ != nullptr) params_.query_ctx_->disable_plan_cache_ = true;
  int ret=enabled(*session_info_); if (OB_FAIL(ret)) return ret;
  try {
    FileOptions o; ret=options(call,o); if (OB_FAIL(ret)) return ret;
    if (o.format=="glob") {
      ret=glob_pattern(o,*session_info_); FileColumn c; c.name="file"; c.type=TEXT; o.columns.push_back(std::move(c));
    } else ret=infer_schema(o,*session_info_);
    if (OB_FAIL(ret)) return ret;
    TableFunctionCall hidden=call; hidden.arguments_=nullptr;
    ret=create_function_scan_item(hidden,item); if (OB_FAIL(ret)) return ret;
    const std::string encoded=encode(o); ObString persistent;
    ret=ob_write_string(*allocator_,str(encoded),persistent); if (OB_FAIL(ret)) return ret;
    ObConstRawExpr *arg=nullptr;
    ret=ObRawExprUtils::build_const_string_expr(*params_.expr_factory_,ObVarcharType,persistent,CS_TYPE_UTF8MB4_BIN,arg);
    if (OB_SUCC(ret)) ret=static_cast<ObOpRawExpr *>(item->function_table_expr_)->add_param_expr(arg);
    for (size_t i=0;OB_SUCC(ret) && i<o.columns.size();++i) {
      const auto &c=o.columns[i]; ObObjMeta meta; ObAccuracy accuracy=ObAccuracy::MAX_ACCURACY[ObVarcharType];
      switch(c.type) {
        case BOOL: meta.set_tinyint(); accuracy.set_precision(1); accuracy.set_scale(0); break;
        case INT: meta.set_int(); accuracy=ObAccuracy::MAX_ACCURACY[ObIntType]; break;
        case REAL: meta.set_double(); accuracy=ObAccuracy::DDL_DEFAULT_ACCURACY[ObDoubleType]; break;
        case DATE: meta.set_date(); accuracy=ObAccuracy::MAX_ACCURACY[ObDateType]; break;
        case DATETIME: meta.set_datetime(); accuracy=ObAccuracy::MAX_ACCURACY[ObDateTimeType]; accuracy.set_scale(c.scale); break;
        default: meta.set_varchar(); meta.set_collation_type(CS_TYPE_UTF8MB4_BIN); accuracy.set_length(MAX_RECORD); break;
      }
      ObString name; ret=ob_write_string(*allocator_,str(c.name),name); ColumnItem *bound=nullptr;
      if (OB_SUCC(ret)) ret=resolve_function_table_column_item(*item,meta,accuracy,name,OB_APP_MIN_COLUMN_ID+i,bound);
      if (OB_SUCC(ret) && !c.nullable) {
        ObRawExprResType result=bound->expr_->get_result_type(); result.set_result_flag(NOT_NULL_FLAG); bound->expr_->set_result_type(result);
      }
    }
    if (OB_SUCC(ret)) ret=item->function_table_expr_->extract_info();
    return ret;
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}
namespace {
struct CopyOptions {
  bool header = false;
  char delimiter = ',', quote = '"', escape = '"';
  std::string nullstr;
  std::vector<std::string> names;
};
class FileCopySink final : public IFileCopySink {
public:
  explicit FileCopySink(ObExecContext &ctx) : ctx_(ctx) {}
  ~FileCopySink() override {
    if (fd_ >= 0) ::close(fd_);
    if (dir_ >= 0) {
      if (!temporary_.empty()) unlinkat(dir_, temporary_.c_str(), 0);
      ::close(dir_);
    }
  }
  int open(const ObString &target, const ObString &encoded) {
    std::istringstream in(std::string(encoded.ptr(), encoded.length()));
    std::string marker; int delimiter, quote, escape; size_t columns;
    if (!(in >> marker >> options_.header >> delimiter >> quote >> escape
             >> std::quoted(options_.nullstr) >> columns) || marker != "FILE_SQL_COPY"
        || columns > MAX_COLUMNS) return error("invalid COPY plan");
    options_.delimiter = delimiter; options_.quote = quote; options_.escape = escape;
    for (size_t i = 0; i < columns; ++i) {
      std::string name; if (!(in >> std::quoted(name))) return error("invalid COPY header");
      options_.names.push_back(std::move(name));
    }
    std::string path(target.ptr(), target.length());
    const size_t slash = path.rfind('/');
    if (path.empty() || path.find('\0') != std::string::npos) return error("invalid COPY path");
    name_ = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name_.empty() || name_ == "." || name_ == "..") return error("invalid COPY target");
    std::string parent = slash == std::string::npos ? "." : path.substr(0, slash + 1);
    char canonical[PATH_MAX];
    if (!realpath(parent.c_str(), canonical)) return error("COPY directory does not exist", parent);
    int ret = allowed(*ctx_.get_my_session(), canonical); if (OB_FAIL(ret)) return ret;
    dir_ = ::open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir_ < 0) return error("cannot open COPY directory", parent);
    // Check the pinned directory after opening, closing rename/symlink races.
    char actual[PATH_MAX]; std::string proc = "/proc/self/fd/" + std::to_string(dir_);
    if (!realpath(proc.c_str(), actual)) return error("cannot resolve COPY directory");
    ret = allowed(*ctx_.get_my_session(), actual); if (OB_FAIL(ret)) return ret;
    struct stat st;
    if (fstatat(dir_, name_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
      return error("COPY target already exists or is inaccessible", path);
    for (unsigned attempt = 0; attempt < 64 && fd_ < 0; ++attempt) {
      temporary_ = ".seekdb-copy-" + std::to_string(getpid()) + "-" +
          std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" + std::to_string(attempt);
      fd_ = openat(dir_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (fd_ < 0 && errno != EEXIST) { temporary_.clear(); return error("cannot create COPY temporary file"); }
      if (fd_ < 0) temporary_.clear(); // Never unlink a file owned by another writer.
    }
    if (fd_ < 0) return error("COPY temporary name exhausted");
    if (options_.header) {
      for (size_t i = 0; OB_SUCC(ret) && i < options_.names.size(); ++i) {
        if (i) ret = append(std::string(1, options_.delimiter));
        if (OB_SUCC(ret)) ret = field(options_.names[i], true);
      }
      if (OB_SUCC(ret)) ret = append("\n");
    }
    return ret;
  }
  int write(const ObIArray<ObObj> &row) override {
    try {
      if (row.count() != static_cast<int64_t>(options_.names.size())) return error("COPY column count mismatch");
      int ret = OB_SUCCESS;
      ObObjPrintParams params; params.tz_info_ = ctx_.get_my_session()->get_timezone_info();
      params.cs_type_ = CS_TYPE_UTF8MB4_BIN;
      for (int64_t i = 0; OB_SUCC(ret) && i < row.count(); ++i) {
        if (i) ret = append(std::string(1, options_.delimiter));
        if (OB_FAIL(ret)) break;
        const ObObj &obj = row.at(i);
        if (obj.is_null()) ret = append(options_.nullstr);
        else if (obj.is_string_type() && !obj.is_lob_storage()) {
          ObString value = obj.get_string();
          if (value.length() > MAX_RECORD) return error("COPY field exceeds resource limit");
          ret = field(std::string(value.ptr(), value.length()), true);
        } else {
          size_t capacity = 256; int64_t pos = 0;
          do {
            buffer_.resize(capacity); pos = 0;
            ret = obj.print_plain_str_literal(buffer_.data(), buffer_.size(), pos, params);
            if (ret != OB_SIZE_OVERFLOW) break;
            capacity *= 2;
          } while (capacity <= MAX_RECORD);
          if (OB_SUCC(ret)) ret = field(std::string(buffer_.data(), pos), false);
        }
      }
      if (OB_SUCC(ret)) ret = append("\n");
      return ret;
    } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
  }
  int publish() override {
    if (fd_ < 0 || published_) return OB_ERR_UNEXPECTED;
    int ret = ctx_.check_status(); if (OB_FAIL(ret)) return ret;
    char actual[PATH_MAX]; std::string proc = "/proc/self/fd/" + std::to_string(dir_);
    if (!realpath(proc.c_str(), actual)) return error("COPY directory changed");
    ret = allowed(*ctx_.get_my_session(), actual); if (OB_FAIL(ret)) return ret;
    if (fsync(fd_) != 0) return error("COPY flush failed");
    // Same-directory hard link publishes atomically and cannot replace a target.
    if (linkat(dir_, temporary_.c_str(), dir_, name_.c_str(), 0) != 0)
      return error("COPY publish failed; target may already exist");
    published_ = true;
    unlinkat(dir_, temporary_.c_str(), 0); temporary_.clear();
    return OB_SUCCESS;
  }
private:
  int append(const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
      ssize_t count = ::write(fd_, text.data() + offset, text.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return error("COPY write failed");
      offset += count;
    }
    return OB_SUCCESS;
  }
  int field(const std::string &text, bool string) {
    const bool quoted = string || text == options_.nullstr ||
        text.find_first_of(std::string() + options_.delimiter + options_.quote + "\r\n") != std::string::npos;
    if (!quoted) return append(text);
    std::string out; out.reserve(text.size() + 2); out += options_.quote;
    for (char c : text) {
      if (c == options_.quote || c == options_.escape) out += options_.escape;
      out += c;
    }
    out += options_.quote; return append(out);
  }
  ObExecContext &ctx_; CopyOptions options_;
  int dir_ = -1, fd_ = -1; bool published_ = false;
  std::string name_, temporary_; std::vector<char> buffer_;
};
} // namespace

int resolve_file_copy(const ParseNode &node, ObSQLSessionInfo &session, ObSelectStmt &stmt,
                      ObIAllocator &allocator, ObString &encoded)
{
  int ret = enabled(session); if (OB_FAIL(ret)) return ret;
  try {
    CopyOptions o; std::vector<std::string> seen;
    const ParseNode *list = node.children_[1];
    for (int64_t i = 0; list && i < list->num_child_; ++i) {
      const ParseNode *option = list->children_[i];
      if (!option || option->num_child_ != 2) return error("invalid COPY option");
      const std::string key = lower(node_string(option->children_[0]));
      const ParseNode *value = option->children_[1]; std::string text = node_string(value);
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) return error("duplicate COPY option");
      seen.push_back(key);
      if (key == "format") {
        if (!value || lower(text) != "csv") return error("COPY supports only CSV");
      } else if (key == "header") {
        if (!value) o.header = true;
        else if (value->type_ == T_BOOL) o.header = value->value_ != 0;
        else if (lower(text) == "true" || lower(text) == "false") o.header = lower(text) == "true";
        else return error("COPY header must be boolean");
      } else if (key == "nullstr" || key == "null") {
        if (!value || value->type_ != T_VARCHAR) return error("COPY nullstr must be text");
        o.nullstr = text;
      } else if (key == "delimiter" || key == "delim" || key == "sep" || key == "quote" || key == "escape") {
        if (!value || value->type_ != T_VARCHAR || text.size() != 1 ||
            text[0] == '\0' || text[0] == '\r' || text[0] == '\n') return error("invalid COPY CSV character");
        if (key == "quote") o.quote = text[0]; else if (key == "escape") o.escape = text[0]; else o.delimiter = text[0];
      } else return error("unknown COPY option");
    }
    if (o.delimiter == o.quote || o.nullstr.find_first_of(std::string() + o.delimiter + o.quote + "\r\n") != std::string::npos)
      return error("ambiguous COPY CSV options");
    std::ostringstream out; out << "FILE_SQL_COPY " << o.header << ' ' << int(o.delimiter) << ' '
        << int(o.quote) << ' ' << int(o.escape) << ' ' << std::quoted(o.nullstr) << ' ' << stmt.get_select_item_size();
    for (int64_t i = 0; i < stmt.get_select_item_size(); ++i) {
      const SelectItem &item = stmt.get_select_item(i);
      const ObString &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      out << ' ' << std::quoted(std::string(name.ptr(), name.length()));
    }
    return ob_write_string(allocator, str(out.str()), encoded);
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}
int create_file_copy_sink(ObExecContext &ctx, const ObString &path, const ObString &encoded, IFileCopySink *&sink)
{
  sink = nullptr;
  try {
    std::unique_ptr<FileCopySink> candidate(new FileCopySink(ctx));
    int ret = candidate->open(path, encoded);
    if (OB_SUCC(ret)) sink = candidate.release();
    return ret;
  } catch (const std::bad_alloc &) { return OB_ALLOCATE_MEMORY_FAILED; }
}

} } // namespace oceanbase::sql
