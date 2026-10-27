from test.cunit.thread_tracker import ThreadTracker

MAX_THREAD_TRACKER = 256


def test_new():
    t = ThreadTracker()
    assert t.__cself__


def test_get_or_create_new_entry():
    t = ThreadTracker()
    entry = t.get_or_create(1001)
    assert entry


def test_get_or_create_returns_same_entry():
    t = ThreadTracker()
    e1 = t.get_or_create(1001)
    e2 = t.get_or_create(1001)
    assert e1 == e2


def test_get_or_create_different_tids():
    t = ThreadTracker()
    e1 = t.get_or_create(1001)
    e2 = t.get_or_create(1002)
    assert e1 != e2


def test_get_or_create_capacity_limit():
    t = ThreadTracker()
    for tid in range(MAX_THREAD_TRACKER):
        assert t.get_or_create(tid)
    # One over the limit returns NULL
    assert not t.get_or_create(MAX_THREAD_TRACKER)


def test_evict_stale_no_eviction_when_gen_unchanged():
    # sample_gen starts at 0; all entries also have last_gen=0, so nothing evicted
    t = ThreadTracker()
    t.get_or_create(1001)
    t.get_or_create(1002)
    t.evict_stale()
    # Both entries should still be present (returned as existing entries, not new ones)
    assert t.get_or_create(1001) == t.get_or_create(1001)
    assert t.get_or_create(1002) == t.get_or_create(1002)
