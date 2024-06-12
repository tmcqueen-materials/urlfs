Read-only FUSE Filesystem Accessing URL Resources
=========================

Based on the [Index Filesystem for FUSE](https://github.com/MajenkoProjects/indexfs),
but with the following changes:

- Filesystem made purely read only
- A config file is now added (and unchanged by mount.urlfs)
- The index file is not changed (i.e. no file sizes saved) on unmounting
- Support for passing headers, e.g. for authorization, is added

The format of the config file is text based. The first non-empty line is the index file:

```
/path/to/index-file
```

Followed by zero or more header lines exactly as they should be provided to curl_slist_append:

```
X-Auth-Token: SomeLongToken
```

The format of the index file is text based and consists of rows of:

```
D<tab>Directory name
```

And

```
F<tab>File name<tab>URL
```

To execute:

```
mount.urlfs [-o options] /path/to/config /mount/point
```

If the config file doesn't exist you will have an empty filesystem.

----

----

URLs
----

Thanks to CURL being the backend almost any standard URI works. For example:

* http://path and https://path
* file:///path
* ftp://user:password@site/path

Obviously headers only make sense in some contexts.

