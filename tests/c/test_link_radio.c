/* Hub <-> remote over the fake radio (tests/c/fake_radio.h): measured CC2544 behaviour, real hub and remote links.
 * Sessions S2 / S3 are described there. */
#include "fake_radio.h"

static hp_rf_msg_t pages[8];
static int npages, ntiles;

static void count_msgs(const hp_rf_msg_t *m) {
    if (m->type == HP_MSG_PAGE_PUSH && npages < 8) pages[npages++] = *m;
    if (m->type == HP_MSG_TILE_UPDATE) ntiles++;
}

/* 56 chars = 42 bytes: page_push body of 3 + 42 + header spans two RF frames */
#define B64_A "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFB"
#define B64_B "QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJC"

static void test_uplink_acked_without_retries_with_polling(model_t m) {
    world_init(true, m);
    run(1000); /* polls bring the link up */
    CT_CHECK(R.up);
    for (int i = 0; i < 5; i++) { /* five "key presses", one after another */
        int before = R.remote_objs_sent;
        CT_EQ(send_uplink(), 0);
        run(120); /* well under the 150 ms retry */
        CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* acked */
        CT_EQ(R.remote_objs_sent - before, 1);     /* ... on the first transmission */
    }
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 5u);
    CT_EQ(R.hub_acks_written, 5);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_without_polling_the_ack_waits_for_the_retry(void) { /* S2 only */
    world_init(false, MODEL_S2);
    CT_EQ(send_uplink(), 0);
    run(100);
    CT_EQ(hp_rf_link_status(&RL).queued, 1u); /* the ack is stuck in the radio until the remote speaks again */
    run(200);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u); /* the retry carried it */
}

static void test_two_frame_page_delivered_after_link_up(model_t m) {
    world_init(true, m);
    run(400);
    CT_CHECK(R.up);
    CT_CHECK(push_page(3, B64_A));
    run(600);
    CT_EQ(npages, 1);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u); /* and acked */
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
    CT_EQ(R.hub_data_written, 2); /* two frames, no retry */
    CT_EQ(pages[0].payload[pages[0].len - 1], 'A');
    CT_CHECK(R.max_depth <= 1);
}

static void test_page_pushed_while_link_down_is_sent_on_link_up(model_t m) {
    world_init(true, m);
    run(400);
    hp_rf_link_set_awake(&RL, false, R.now); /* remote goes quiet: link lost after 500 ms */
    run(800);
    CT_CHECK(!R.up);
    CT_CHECK(push_page(3, B64_A)); /* plain ok, held */
    CT_CHECK(push_page(3, B64_B)); /* the latest per (remote,page) wins */
    CT_EQ(H.npend, 1);
    run(300);
    CT_EQ(npages, 0);
    CT_EQ(R.hub_data_written, 0); /* nothing written to a dead link */
    hp_rf_link_set_awake(&RL, true, R.now); /* the remote wakes and polls */
    run(800);
    CT_CHECK(R.up);
    CT_EQ(H.npend, 0);
    CT_EQ(npages, 1);
    CT_EQ(pages[0].payload[pages[0].len - 1], 'B');
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(R.hub_writes_while_down, 0);
    CT_CHECK(R.max_depth <= 1);
}

static void test_no_duplicate_ack_pile_up(model_t m) {
    world_init(true, m);
    run(400);
    R.drop_dl = 3; /* the first acks are lost on the air: the remote retries the object */
    CT_EQ(send_uplink(), 0);
    run(900);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 1u); /* retries are duplicates, not redeliveries */
    CT_CHECK(R.max_depth <= 1);
    /* at most one ack per transmission of the object the hub saw, never a burst of stale ones */
    CT_CHECK(R.hub_acks_written <= R.remote_objs_sent);
    run(2000);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_link_flap_keeps_queue_and_sends_in_order(model_t m) {
    world_init(true, m);
    run(400);
    CT_CHECK(push_page(1, B64_A));
    hp_rf_link_set_awake(&RL, false, R.now); /* link drops mid-delivery */
    run(40);
    hp_rf_link_set_awake(&RL, true, R.now);
    run(1500);
    CT_EQ(npages, 1);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_CHECK(R.max_depth <= 1);
}

/* ---- S3 only: the radio delivers by itself, so only an uplink frame proves delivery ---- */

static void test_s3_key_pair_acked_first_try(void) {
    world_init(true, MODEL_S3);
    run(1000);
    CT_CHECK(R.up);
    for (int i = 0; i < 2; i++) { /* the second object goes right after the first is acked */
        CT_EQ(send_uplink(), 0);
        uint32_t t0 = R.now;
        while (hp_rf_link_status(&RL).queued && R.now - t0 < 300) run(5);
        CT_CHECK(R.now - t0 <= 100);
        CT_EQ(R.remote_objs_sent, i + 1); /* first try */
    }
    CT_EQ(R.hub_acks_written, 2);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_CHECK(R.max_depth <= 1);
    CT_CHECK(R.max_lat <= 15);
}

static void test_s3_two_frame_page_fast(void) {
    world_init(true, MODEL_S3);
    run(400);
    CT_CHECK(push_page(3, B64_A));
    uint32_t t0 = R.now;
    while ((npages < 1 || hp_rf_link_status(&H.link).queued) && R.now - t0 < 600) run(5);
    CT_EQ(npages, 1);
    CT_CHECK(R.now - t0 <= 120);
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
}

static void test_s3_asleep_remote_falls_back_to_timeout(void) {
    world_init(true, MODEL_S3);
    run(400);
    while (R.now - R.last_uplink >= 5) run(5); /* just after a poll: the link stays up for 500 ms more */
    hp_rf_link_set_awake(&RL, false, R.now);   /* no more polls */
    CT_CHECK(R.up);
    CT_CHECK(push_page(3, B64_A));
    run(400);
    CT_EQ(npages, 1); /* frame 2 left on the 300 ms timeout */
    CT_EQ(hp_rf_link_status(&H.link).queued, 0u);
    CT_EQ(hp_rf_link_status(&H.link).failures, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_CHECK(R.max_depth <= 1);
    CT_EQ(R.hub_writes_while_down, 0);
}

static void test_s3_dropped_downlink_dedup(void) {
    world_init(true, MODEL_S3);
    run(400);
    R.drop_dl = 2;
    CT_EQ(send_uplink(), 0);
    run(1500);
    CT_EQ(hp_rf_link_status(&RL).queued, 0u);
    CT_EQ(hp_rf_link_status(&RL).failures, 0u);
    CT_EQ(hp_rf_link_status(&H.link).delivered, 1u);
    CT_CHECK(R.hub_acks_written <= R.remote_objs_sent);
    CT_CHECK(R.max_depth <= 1);
}

/* An uplink that arrived BEFORE the write but is read after it (late reader) must not release the frame. */
static void test_late_read_of_pre_write_uplink_does_not_release(model_t m) {
    world_init(true, m);
    run(1000);
    R.hold_rx = true; /* the hub's reader is behind */
    int polls = R.remote_polls_sent;
    while (R.remote_polls_sent == polls) run(5); /* a poll arrives, unread */
    CT_CHECK(push_page(3, B64_A));
    run(5); /* the hub writes frame 1 */
    CT_EQ(R.hub_data_written, 1);
    R.hold_rx = false;
    run(5); /* the stale uplink is read 5 ms after the write */
    CT_EQ(R.hub_data_written, 1);
    CT_CHECK(R.max_depth <= 1);
    run(1500);
    CT_EQ(npages, 1);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
}

/* An uplink arriving within the margin after the write does not release; one after the margin does. */
static void test_uplink_margin(model_t m) {
    for (int late = 0; late < 2; late++) {
        world_init(true, m);
        run(1000);
        CT_CHECK(push_page(3, B64_A));
        run(5); /* frame 1 written at R.now */
        CT_EQ(R.hub_data_written, 1);
        uint32_t t_w = R.now;
        if (late) run(UPLINK_PREP_MS); /* arrives exactly one margin after the write */
        CT_EQ(send_uplink(), 0);
        (void)t_w;
        run(5); /* the hub reads it */
        CT_EQ(R.hub_acks_written, late ? 1 : 0); /* the uplink's ack goes out at once only if it released the slot */
        CT_EQ(R.hub_data_written, 1);
        CT_CHECK(R.max_depth <= 1);
        run(1500);
        CT_EQ(npages, 1);
        CT_CHECK(R.max_depth <= 1);
    }
}

/* A transport without frame_times: no uplink release, the 300 ms timeout (or a status 00) only. */
static void test_without_stamps_release_is_by_timeout(void) {
    world_init(true, MODEL_S3);
    R.hub_t.frame_times = NULL;
    run(400);
    CT_CHECK(push_page(3, B64_A));
    uint32_t t0 = R.now;
    while (npages < 1 && R.now - t0 < 800) run(5);
    CT_EQ(npages, 1);
    CT_CHECK(R.now - t0 >= (uint32_t)HP_LINK_OUT_TIMEOUT_MS);
    CT_EQ(R.hub_data_written, 2);
    CT_CHECK(R.max_depth <= 1);
}

static void reset_counts(void) { npages = ntiles = 0; }

int main(void) {
    remote_hook = count_msgs;
    world_reset_hook = reset_counts;
    for (model_t m = MODEL_S2; m <= MODEL_S3; m++) {
        test_uplink_acked_without_retries_with_polling(m);
        test_two_frame_page_delivered_after_link_up(m);
        test_page_pushed_while_link_down_is_sent_on_link_up(m);
        test_no_duplicate_ack_pile_up(m);
        test_link_flap_keeps_queue_and_sends_in_order(m);
        test_late_read_of_pre_write_uplink_does_not_release(m);
        test_uplink_margin(m);
    }
    test_without_polling_the_ack_waits_for_the_retry();
    test_without_stamps_release_is_by_timeout();
    test_s3_key_pair_acked_first_try();
    test_s3_two_frame_page_fast();
    test_s3_asleep_remote_falls_back_to_timeout();
    test_s3_dropped_downlink_dedup();
    return CT_DONE();
}
