/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef KVROW_HH
#define KVROW_HH 1
#include "vec.hh"
#include "kvio.hh"
#include "misc.hh"
#include "kvthread.hh"
#include "kvproto.hh"
#include "log.hh"
#include <algorithm>

template <typename IDX>
struct row_base {
    struct cell_t {
        typename IDX::field_t c_fid;
        Str c_value;
	friend bool operator<(const cell_t &a, const cell_t &b) {
	    return a.c_fid < b.c_fid;
	}
    };
    typedef KUtil::vec<cell_t> change_t;
    typedef KUtil::vec<typename IDX::field_t> fields_t;
    static int parse_change(Str v, change_t &c) {
        struct kvin kvin;
        kvin_init(&kvin, const_cast<char *>(v.s), v.len);
        return kvread_change(&kvin, c);
    }
    static int parse_fields(Str v, fields_t &f) {
	struct kvin kvin;
        kvin_init(&kvin, const_cast<char *>(v.s), v.len);
        return kvread_fields(&kvin, f);
    }
    static int kvread_fields(struct kvin *kvin, fields_t &f) {
        short n;
        KVR(kvin, n);
        f.resize(n);
        for (short i = 0; i < n; i++)
            IDX::kvread_field(kvin, f[i]);
        return 0;
    }
    static int kvwrite_fields(struct kvout *kvout, const fields_t &f) {
        short n = f.size();
	for (short i = 1; i < n; i++)
	    if (!(f[i - 1] < f[i])) {
	        assert(0 && "The fields must be sorted");
		exit(EXIT_FAILURE);
	    }
        KVW(kvout, n);
        for (short i = 0; i < n; i++)
            IDX::kvwrite_field(kvout, f[i]);
        return 0;
    }
    static int kvread_change(struct kvin *kvin, change_t &c) {
        short n;
        KVR(kvin, n);
        c.resize(n);
        for (short i = 0; i < n; i++) {
            IDX::kvread_field(kvin, c[i].c_fid);
            kvread_str_inplace(kvin, c[i].c_value);
        }
        return 0;
    }
    static void sort(change_t &c) {
	std::sort(c.begin(), c.end());
    }
    static void sort(fields_t &f) {
	std::sort(f.begin(), f.end());
    }
    static int kvwrite_change(struct kvout *kvout, const change_t &c) {
        short n = c.size();
	for (short i = 1; i < n; i++)
	    if (!(c[i - 1] < c[i])) {
	        assert(0 && "The change must be sorted");
		exit(EXIT_FAILURE);
	    }
        KVW(kvout, n);
        for (short i = 0; i < n; i++) {
            IDX::kvwrite_field(kvout, c[i].c_fid);
            kvwrite_str(kvout, c[i].c_value);
        }
        return 0;
    }

    static cell_t make_cell(typename IDX::field_t fid, Str value) {
	cell_t c;
	c.c_fid = fid;
	c.c_value = value;
	return c;
    }

    /** @brief Interfaces for column-less key/value store. */
    static void make_get1_fields(fields_t &f) {
        f.resize(1);
        IDX::make_full_field(f[0]);
    }
    static void make_put1_change(change_t &c, Str val) {
        c.resize(1);
        IDX::make_full_field(c[0].c_fid);
        c[0].c_value = val;
    }
    static Str make_put_col_request(struct kvout *kvout,
				    typename IDX::field_t fid,
				    Str value) {
	kvout_reset(kvout);
	KVW(kvout, short(1));
	IDX::kvwrite_field(kvout, fid);
	kvwrite_str(kvout, value);
	return Str(kvout->buf, kvout->n);
    }
};


template <typename R>
struct query_helper {
    inline const R* snapshot(const R* row, const typename R::fields_t&, threadinfo&) {
        return row;
    }
};

template <typename R>
struct query {
    enum {
	QT_None = 0,
	QT_Get = 1,
	QT_Scan = 2,
	QT_Ckp_Scan = 3,
	QT_Get1_Col0 = 4, /* + column index */

	QT_Put = 4,
	QT_Remove = 5,
	QT_MinReplay = 7,
	QT_Replay_Put = 7,
	QT_Replay_Remove = 8,
	QT_Replay_Modify = 9
    };

    void begin_get(Str key, Str req, struct kvout* kvout);
    void begin_put(Str key, Str req);
    void begin_remove(Str key);
    void begin_scan(Str startkey, int npairs, Str req,
		    struct kvout* kvout);
    void begin_checkpoint(ckstate* ck, Str startkey, Str endkey);

    /** @brief interfaces where the value is a single column,
     *    and where "get" does not emit but save a copy locally.
     */
    void begin_get1(Str key, int col = 0) {
	qt_ = QT_Get1_Col0 + col;
	key_ = key;
    }
    Str get1_value() const {
	return val_;
    }
    void begin_scan1(Str startkey, int npairs, kvout* kv);
    void begin_put1(Str key, Str value);

    int query_type() const {
	return qt_;
    }
    const loginfo::query_times& query_times() const {
        return qtimes_;
    }

    inline bool emitrow(const R* v, threadinfo* ti);
    /** @return whether the scan should continue or not
     */
    bool scanemit(Str k, const R* v, threadinfo* ti);

    inline result_t apply_put(R*& value, bool has_value, threadinfo* ti);
    inline bool apply_remove(R*& value, bool has_value, threadinfo* ti, kvtimestamp_t* node_ts = 0);

  private:
    typename R::change_t c_;
    typename R::fields_t f_;
    unsigned long scan_npairs_;
    loginfo::query_times qtimes_;
  public:
    Str key_;   // startkey for scan; key for others
    kvout* kvout_;
    query_helper<R> helper_;
  private:
    int qt_;    // query type
    ckstate* ck_;
    Str endkey_;
    Str val_;			// value for Get1 and CkpPut
    void emit(const R* row);
    void assign_timestamp(threadinfo* ti);
    void assign_timestamp(threadinfo* ti, kvtimestamp_t t);
};

template <typename R>
void query<R>::begin_get(Str key, Str req, kvout* kv) {
    qt_ = QT_Get;
    key_ = key;
    R::parse_fields(req, f_);
    kvout_ = kv;
}

template <typename R>
void query<R>::begin_put(Str key, Str req) {
    qt_ = QT_Put;
    key_ = key;
    R::parse_change(req, c_);
}

template <typename R>
void query<R>::begin_put1(Str key, Str val) {
    qt_ = QT_Put;
    key_ = key;
    R::make_put1_change(c_, val);
}

template <typename R>
void query<R>::begin_scan(Str startkey, int npairs, Str req, kvout* kv) {
    assert(npairs > 0);
    qt_ = QT_Scan;
    key_ = startkey;
    R::parse_fields(req, f_);
    scan_npairs_ = npairs;
    kvout_ = kv;
}

template <typename R>
void query<R>::begin_scan1(Str startkey, int npairs, kvout* kv) {
    assert(npairs > 0);
    qt_ = QT_Scan;
    key_ = startkey;
    R::make_get1_fields(f_);
    scan_npairs_ = npairs;
    kvout_ = kv;
}

template <typename R>
void query<R>::begin_checkpoint(ckstate* ck, Str startkey, Str endkey) {
    qt_ = QT_Ckp_Scan;
    key_ = startkey;
    ck_ = ck;
    endkey_ = endkey;
}

template <typename R>
void query<R>::begin_remove(Str key) {
    qt_ = QT_Remove;
    key_ = key;
}

template <typename R>
void query<R>::emit(const R* row) {
    if (f_.size() == 0) {
        KVW(kvout_, (short) row->ncol());
        for (int i = 0; i != row->ncol(); ++i)
            kvwrite_str(kvout_, row->col(i));
    } else {
        KVW(kvout_, (short) f_.size());
        for (int i = 0; i != (int) f_.size(); ++i)
            kvwrite_str(kvout_, row->col(f_[i]));
    }
}

template <typename R>
bool query<R>::scanemit(Str k, const R* v, threadinfo* ti) {
    if (row_is_marker(v))
	return true;
    if (qt_ == QT_Ckp_Scan) {
        if (endkey_ && k >= endkey_)
            return false;
        ::checkpoint1(ck_, k, v);
        return true;
    } else {
        assert(qt_ == QT_Scan);
	kvwrite_str(kvout_, k);
        emit(helper_.snapshot(v, f_, *ti));
        --scan_npairs_;
        return scan_npairs_ > 0;
    }
}

template <typename R>
inline bool query<R>::emitrow(const R* v, threadinfo* ti) {
    if (row_is_marker(v))
	return false;
    else if (qt_ >= QT_Get1_Col0) {
	val_ = v->col(qt_ - QT_Get1_Col0);
	return true;
    } else {
        assert(qt_ == QT_Get);
        emit(helper_.snapshot(v, f_, *ti));
	return true;
    }
}

template <typename R>
inline void query<R>::assign_timestamp(threadinfo *ti) {
    qtimes_.ts = ti->update_timestamp();
    qtimes_.prev_ts = 0;
}

template <typename R>
inline void query<R>::assign_timestamp(threadinfo* ti, kvtimestamp_t min_ts) {
    qtimes_.ts = ti->update_timestamp(min_ts);
    qtimes_.prev_ts = min_ts;
}

template <typename R>
inline result_t query<R>::apply_put(R*& value, bool has_value, threadinfo* ti) {
    precondition(qt_ < QT_MinReplay);

    if (loginfo* log = ti->ti_log) {
	log->acquire();
	qtimes_.epoch = global_log_epoch;
    }

    if (!has_value) {
    insert:
	assign_timestamp(ti);
        value = R::from_change(c_, qtimes_.ts, *ti);
	return Inserted;
    }

    R* old_value = value;
    assign_timestamp(ti, old_value->ts_);
    if (row_is_marker(old_value)) {
	old_value->deallocate_rcu(*ti);
	goto insert;
    }

    R* updated = old_value->update(c_, qtimes_.ts, *ti);
    if (updated != old_value) {
	value = updated;
	old_value->deallocate_rcu_after_update(c_, *ti);
    }
    return Updated;
}

template <typename R>
inline bool query<R>::apply_remove(R *&value, bool has_value, threadinfo *ti,
				   kvtimestamp_t *node_ts) {
    if (!has_value)
	return false;

    if (loginfo* log = ti->ti_log) {
	log->acquire();
	qtimes_.epoch = global_log_epoch;
    }

    R* old_value = value;
    assign_timestamp(ti, old_value->ts_);
    if (node_ts && circular_int<kvtimestamp_t>::less_equal(*node_ts, qtimes_.ts))
	*node_ts = qtimes_.ts + 2;
    old_value->deallocate_rcu(*ti);
    return true;
}


template <typename R>
struct query_scanner {
    query<R> &q_;
    query_scanner(query<R> &q)
	: q_(q) {
    }
    bool operator()(Str key, R* value, threadinfo* ti) {
	return q_.scanemit(key, value, ti);
    }
};

#include KVDB_ROW_TYPE_INCLUDE
#endif
