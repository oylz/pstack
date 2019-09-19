#include "libpstack/util.h"

#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <iostream>

using std::string;

string
linkResolve(string name)
{
    char buf[1024];
    int rc;
    for (;;) {
        rc = readlink(name.c_str(), buf, sizeof buf - 1);
        if (rc == -1)
            break;
        buf[rc] = 0;
        if (buf[0] != '/') {
            auto lastSlash = name.rfind('/');
            name = lastSlash == string::npos ? string(buf) : name.substr(0, lastSlash + 1) + string(buf);
        } else {
            name = buf;
        }
    }
    return name;
}

off_t
FileReader::size() const
{
    if (fileSize == -1) {
       struct stat buf{};
       int rc = fstat(file, &buf);
       if (rc == -1)
           throw (Exception() << "fstat failed: can't find size of file: " << strerror(errno));
       fileSize = buf.st_size;
    }
    return fileSize;
}

static bool
openFileDirect(int &file, const std::string &name_)
{
    auto fd = open(name_.c_str(), O_RDONLY);
    if (fd != -1) {
        file = fd;
        if (verbose > 1)
           *debug << "opened " << name_ << ", fd=" << file << std::endl;
        return true;
    }
     if (verbose > 1)
        *debug << "failed to open " << name_ << ": " << strerror(errno) << std::endl;
    return false;
}

bool
FileReader::openfile(int &file, const std::string &name_)
{
    name = name_;
    if (g_openPrefix != "" && openFileDirect(file, g_openPrefix + name_)) {
          return true;
    }
    return openFileDirect(file, name_);
}

FileReader::FileReader(string name_)
    : name(std::move(name_))
    , file(-1)
    , fileSize(-1)
{
    if (!openfile(file, name))
        throw (Exception() << "cannot open file '" << name << "': " << strerror(errno));
}

FileReader::~FileReader()
{
    ::close(file);
}

MemReader::MemReader(size_t len_, const char *data_)
    : len(len_)
    , data(data_)
{
}

size_t
MemReader::read(off_t off, size_t count, char *ptr) const
{
    //fprintf(stderr, "MemReader::read\n");
    if (off > off_t(len))
        throw (Exception() << "read past end of memory");
    size_t rc = std::min(count, len - size_t(off));
    memcpy(ptr, data + off, rc);
    return rc;
}

void
MemReader::describe(std::ostream &os) const
{
    os << "in-memory image";
}

std::string
Reader::readString(off_t offset) const
{
    string res;
    for (off_t s = size(); offset < s; ++offset) {
        char c;
        if (read(offset, 1, &c) != 1)
            break;
        if (c == 0)
            break;
        res += c;
    }
    return res;
}

size_t
FileReader::read(off_t off, size_t count, char *ptr) const
{
    //fprintf(stderr, "FileReader::read\n");
    auto rc = pread(file, ptr, count, off);
    if (rc == -1)
        throw (Exception()
            << "read " << count
            << " at " << off
            << " on " << *this
            << " failed: " << strerror(errno));
    return rc;
}

void
CacheReader::Page::load(const Reader &r, off_t offset_)
{
    assert(offset_ % PAGESIZE == 0);
    try {
        len = r.read(offset_, PAGESIZE, data);
        offset = offset_;
    }
    catch (std::exception &ex) {
        len = 0;
    }
}

CacheReader::CacheReader(Reader::csptr upstream_)
    : upstream(move(upstream_))
{
}

CacheReader::~CacheReader()
{
    for (auto &i : pages)
        delete i;
}

CacheReader::Page *
CacheReader::getPage(off_t pageoff) const
{
    Page *p;
    bool first = true;
    for (auto i = pages.begin(); i != pages.end(); ++i) {
        p = *i;
        if (p->offset == pageoff) {
            // move page to front.
            if (!first) {
                pages.erase(i);
                pages.push_front(p);
            }
            return p;
        }
        first = false;
    }
    if (pages.size() == MAXPAGES) {
        p = pages.back();
        pages.pop_back();
    } else {
        p = new Page();
    }
    p->load(*upstream, pageoff);
    pages.push_front(p);
    return p;
}

size_t
CacheReader::read(off_t off, size_t count, char *ptr) const
{
    //fprintf(stderr, "CacheReader::read\n");
    off_t startoff = off;
    for (;;) {
        if (count == 0)
            break;
        size_t offsetOfDataInPage = off % PAGESIZE;
        off_t offsetOfPageInFile = off - offsetOfDataInPage;
        Page *page = getPage(offsetOfPageInFile);
        if (page == nullptr)
            break;
        size_t chunk = std::min(page->len - offsetOfDataInPage, count);
        memcpy(ptr, page->data + offsetOfDataInPage, chunk);
        off += chunk;
        count -= chunk;
        ptr += chunk;
        if (page->len != PAGESIZE)
            break;
    }
    return off - startoff;
}

string
CacheReader::readString(off_t off) const
{
    auto &entry = stringCache[off];
    if (entry.isNew) {
        entry.value = Reader::readString(off);
        entry.isNew = false;
    }
    return entry.value;
}

std::shared_ptr<const Reader>
loadFile(const std::string &path)
{
    return std::make_shared<CacheReader>(
        std::make_shared<FileReader>(path));
}
