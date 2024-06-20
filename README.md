Read-only FUSE Filesystem Accessing URL Resources
=========================

Based on the [Index Filesystem for FUSE](https://github.com/MajenkoProjects/indexfs),
but with the following changes:

- Filesystem made purely read only
- A config file is now added (and unchanged by mount.urlfs)
- The index file(s) are not changed (i.e. no file sizes saved) on unmounting
- Support for passing headers, e.g. for authorization, is added

The format of the config file is text based. Each non-empty line has an index file first,
followed by a tab and zero or more header lines exactly as tey should be provided to
curl_slist_append. These headers are provided when accessing files from that index file:

```
/path/to/index-file<tab>X-Auth-Token: SomeLongToken<tab>X-Header: A Second Header
# This is a comment
/path/to/index2
```

Lines that begin with \# are treated as comments and ignored.

The format of each index file is text based and consists of rows of:

```
D<tab>/Directory name
```

And

```
F<tab>/File/name<tab>URL
```

Sending SIGUSR1 to mount.urlfs causes it to reload all configuration data. It attempts,
to the extent possible, to keep the same file and directory inodes as possible, and to
remember size information if available, so that the reload is transparent to processes
with open file descriptors.

To execute:

```
mount.urlfs [-o options] /path/to/config /mount/point
```

If the config file doesn't exist you will have an empty filesystem.

----

URLs
----

Thanks to CURL being the backend almost any standard URI works. For example:

* http://path and https://path
* file:///path
* ftp://user:password@site/path

Obviously headers only make sense in some contexts.

----

TODO
----

Right now the implementation is naive, storing all files and directories in
a single (potentially very long) linked list. This means many operations are
O(N) instead of O(ln(N)) or O(1), and thus urlfs will be slower than it should
be on filesystems with a large number of files. 

Luckily the primary use case is for O(100's) of files for which this is not
a concern. But a future todo would be to rework the implementation to enable
greater scalability.

