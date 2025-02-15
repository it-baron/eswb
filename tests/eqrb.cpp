#include <string>
#include "tooling.h"
#include "sdtl_tooling.h"
#include "eswb/api.h"

// TODO separate test for internals of EQRB from API based

#include "../src/lib/services/eqrb/eqrb_priv.h"
#include "public/eswb/services/sdtl.h"


TEST_CASE("Topics id map") {

    topic_id_map_t *map;
#define MAX_IDS (100)
    eswb_rv_t rv = map_alloc(&map, MAX_IDS);

#   define LOOKUP(__id) map_find_index(map, (__id), NULL)
#   define ADD_ELEM(__e) map_add_pair(map, (__e), (__e) + 1)

#   define FIRST_SRC_ID 10
#   define SECOND_SRC_ID 11

    SECTION("Map alloc"){
        REQUIRE(map->map != NULL);
    }

    SECTION("Basic container forming and sorting") {
        ADD_ELEM(FIRST_SRC_ID);
        ADD_ELEM(SECOND_SRC_ID);

        REQUIRE(map->records_num == 2);
        REQUIRE(map->map[0].src_topic_id == FIRST_SRC_ID);
        REQUIRE(map->map[1].src_topic_id == SECOND_SRC_ID);
    }

    SECTION("Lookup when no elements") {
        REQUIRE(LOOKUP(123) == eswb_e_map_no_match);
    }

    ADD_ELEM(FIRST_SRC_ID);

    SECTION("Lookup when 1 element") {
        REQUIRE(LOOKUP(FIRST_SRC_ID) == eswb_e_ok);
    }

    ADD_ELEM(SECOND_SRC_ID);

    SECTION("Lookup when 2 elements") {
        REQUIRE(LOOKUP(SECOND_SRC_ID) == eswb_e_ok);
    }

    SECTION("Add existing element to container") {
        rv = ADD_ELEM(FIRST_SRC_ID);
        REQUIRE(rv == eswb_e_map_key_exists);
    }

#   define LEFT_ID 1
    ADD_ELEM(LEFT_ID);

    SECTION("Add element to first index") {
        REQUIRE(map->map[0].src_topic_id == LEFT_ID);
    }

#   define RIGHT_ID 20

    ADD_ELEM(RIGHT_ID);

    SECTION("Add element to last index") {
        REQUIRE(map->map[map->records_num-1].src_topic_id == RIGHT_ID);
    }

    SECTION("Lookup for left elem") {
        REQUIRE(LOOKUP(LEFT_ID) == eswb_e_ok);
    }

    SECTION("Lookup for right elem") {
        REQUIRE(LOOKUP(RIGHT_ID) == eswb_e_ok);
    }

    SECTION("Lookup for absent elem") {
        REQUIRE(LOOKUP(RIGHT_ID-1) == eswb_e_map_no_match);
    }

    eswb_index_t id = RIGHT_ID + 1;

    SECTION("Container at its full") {

        do {
            rv = ADD_ELEM(id);
            id += 2;
        } while (rv == eswb_e_ok);
        REQUIRE(map->records_num == map->size);

        SECTION("Add element to full container") {
            REQUIRE(ADD_ELEM(id+1) == eswb_e_map_full);
        }

        SECTION("Lookup for arbitrary elem in right half") {
            REQUIRE(LOOKUP(73) == eswb_e_ok);
        }

        SECTION("Lookup for arbitrary elem in left half") {
            REQUIRE(LOOKUP(25) == eswb_e_ok);
        }

        SECTION("Lookup for absent elem") {
            REQUIRE(LOOKUP(22) == eswb_e_map_no_match);
        }
    }

    map_dealloc(map);
}

namespace EqrbTestAgent {

class Basic {

public:
    Basic() {
    }

    virtual void start() = 0;
    virtual void stop() = 0;
};

}

void replication_test(EqrbTestAgent::Basic &server, EqrbTestAgent::Basic &client,
                      const std::string &src_bus, const std::string &dst_bus){

    eswb_set_thread_name("main");

    std::string src_bus_full_path = "itb:/" + src_bus;
    std::string dst_bus_full_path = "itb:/" + dst_bus;
    eswb_rv_t erv;
    eqrb_rv_t hrv;

    erv = eswb_create(src_bus.c_str(), eswb_inter_thread, 20);
    REQUIRE(erv == eswb_e_ok);

    erv = eswb_create(dst_bus.c_str(), eswb_inter_thread, 20);
    REQUIRE(erv == eswb_e_ok);

    eswb_topic_descr_t src_bus_td;
    eswb_topic_descr_t dst_bus_td;

    erv = eswb_connect(src_bus_full_path.c_str(), &src_bus_td);
    REQUIRE(erv == eswb_e_ok);

    erv = eswb_connect(dst_bus_full_path.c_str(), &dst_bus_td);
    REQUIRE(erv == eswb_e_ok);

    erv = eswb_event_queue_enable(src_bus_td, 40, 1024);
    REQUIRE(erv == eswb_e_ok);

    eswb_topic_descr_t publisher_td;

    periodic_call_t proclaim = [&] () mutable {
        eswb_rv_t rv;

        TOPIC_TREE_CONTEXT_LOCAL_DEFINE(cntx, 3);

        topic_proclaiming_tree_t *fifo_root = usr_topic_set_fifo(cntx, "fifo", 10);
        usr_topic_add_child(cntx, fifo_root, "cnt", tt_uint32, 0, 4, TOPIC_FLAG_MAPPED_TO_PARENT);

        rv = eswb_event_queue_order_topic(src_bus_td, src_bus.c_str(), 1 );
        thread_safe_failure_assert(rv == eswb_e_ok, "eswb_event_queue_order_topic");

        rv = eswb_proclaim_tree_by_path(src_bus_full_path.c_str(), fifo_root, cntx->t_num, &publisher_td);
        thread_safe_failure_assert(rv == eswb_e_ok, "eswb_proclaim_tree_by_path");

        rv = eswb_event_queue_order_topic(src_bus_td, (src_bus + "/fifo").c_str(), 1 );
        thread_safe_failure_assert(rv == eswb_e_ok, "eswb_event_queue_order_topic");
    };

    uint32_t counter = 0;

    periodic_call_t update = [&] () mutable {
        eswb_rv_t rv = eswb_fifo_push(publisher_td, &counter);
        thread_safe_failure_assert(rv == eswb_e_ok, "eswb_fifo_push");
        counter++;
    };

    periodic_call_t abort = [&] () mutable {
//        FAIL("Timed out abort");
    };

    server.start();
    client.start();

    timed_caller proclaimer(proclaim, 200, "proclaimer");
    timed_caller aborter(abort, 5000, "aborter");
    timed_caller updater(update, 200, "updater");

    proclaimer.start_once(false);
    proclaimer.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds (200));
    updater.start_loop();

    eswb_topic_descr_t replicated_fifo_td;
    erv = eswb_wait_connect_nested(dst_bus_td, "fifo/cnt", &replicated_fifo_td, 2000);
    REQUIRE(erv == eswb_e_ok);

    uint32_t cnt;
    uint32_t expected_cnt = 0;

    aborter.start_once(true);

    do {
        erv = eswb_fifo_pop(replicated_fifo_td, &cnt);
        CHECK(cnt == expected_cnt);
        expected_cnt++;
    } while((erv == eswb_e_ok) && (expected_cnt < 10));

    updater.stop();

    client.stop();
    server.stop();
}

class TestProcess {
    EqrbTestAgent::Basic &server;

    eswb_topic_descr_t src_bus_td;
    eswb_topic_descr_t publisher_td;
    std::string src_bus_full_path;

    periodic_call_t proclaim_call;
    periodic_call_t update_call;

    timed_caller *proclaimer;
    timed_caller *updater;

    std::atomic_uint32_t counter;

public:
    TestProcess(EqrbTestAgent::Basic &server_, const std::string &src_bus) : server(server_) {
        eswb_set_thread_name("main");

        src_bus_full_path = "itb:/" + src_bus;
        eswb_rv_t erv;

        erv = eswb_create(src_bus.c_str(), eswb_inter_thread, 20);
        REQUIRE(erv == eswb_e_ok);

        erv = eswb_connect(src_bus_full_path.c_str(), &src_bus_td);
        REQUIRE(erv == eswb_e_ok);

        erv = eswb_event_queue_enable(src_bus_td, 40, 1024);
        REQUIRE(erv == eswb_e_ok);

        proclaim_call = [&] () mutable {
            eswb_rv_t rv;

            TOPIC_TREE_CONTEXT_LOCAL_DEFINE(cntx, 3);

            topic_proclaiming_tree_t *fifo_root = usr_topic_set_fifo(cntx, "fifo", 10);
            usr_topic_add_child(cntx, fifo_root, "cnt", tt_uint32, 0, 4, TOPIC_FLAG_MAPPED_TO_PARENT);

            rv = eswb_event_queue_order_topic(src_bus_td, src_bus.c_str(), 16 );
            thread_safe_failure_assert(rv == eswb_e_ok, "eswb_event_queue_order_topic");

            rv = eswb_proclaim_tree_by_path(src_bus_full_path.c_str(), fifo_root, cntx->t_num, &publisher_td);
            thread_safe_failure_assert(rv == eswb_e_ok, "eswb_proclaim_tree_by_path");

            rv = eswb_event_queue_order_topic(src_bus_td, (src_bus + "/fifo").c_str(), 16 );
            thread_safe_failure_assert(rv == eswb_e_ok, "eswb_event_queue_order_topic");
        };

        counter = 0;

        update_call = [&] () mutable {
            uint32_t cnt = this->counter;
            eswb_rv_t rv = eswb_fifo_push(publisher_td, &cnt);
            thread_safe_failure_assert(rv == eswb_e_ok, "eswb_fifo_push");
            counter++;
        };

        proclaimer = new timed_caller(proclaim_call, 200, "proclaimer");
        updater = new timed_caller(update_call, 200, "updater");
    }

    void server_start() {
        server.start();
    }

    void do_proclaim() {
        proclaimer->start_once(false);
        proclaimer->wait();
    }

    void start_updater() {
        updater->start_loop();
    }

    void reset_counter() {
        counter = 0;
    }

    void stop() {
        updater->stop();
        server.stop();
    }
};



void replication_test_client(EqrbTestAgent::Basic &client, const std::string &dst_bus){
    std::string dst_bus_full_path = "itb:/" + dst_bus;
    eswb_rv_t erv;

    erv = eswb_create(dst_bus.c_str(), eswb_inter_thread, 20);
    REQUIRE(erv == eswb_e_ok);

    eswb_topic_descr_t dst_bus_td;

    erv = eswb_connect(dst_bus_full_path.c_str(), &dst_bus_td);
    REQUIRE(erv == eswb_e_ok);

    client.start();

    eswb_topic_descr_t replicated_fifo_td;
    erv = eswb_wait_connect_nested(dst_bus_td, "fifo/cnt", &replicated_fifo_td, 2000);
    REQUIRE(erv == eswb_e_ok);

    uint32_t cnt;
    uint32_t expected_cnt = 0;

    do {
        erv = eswb_fifo_pop(replicated_fifo_td, &cnt);
        CHECK(cnt == expected_cnt);
        expected_cnt++;
    } while((erv == eswb_e_ok) && (expected_cnt < 10));

}

extern const eqrb_media_driver_t eqrb_drv_sdtl;


#define EQRB_SDTL_TEST_CHANEL_REL "test_channel_rel"
#define EQRB_SDTL_TEST_CHANEL_UNREL "test_channel_unrel"
extern const sdtl_service_media_t sdtl_test_media;

namespace EqrbTestAgent {

class sdtlBasic : public Basic {

    sdtl_service_t *sdtl_service;
    void *media_data_handle;

    std::string media_path;

protected:
    std::string service_name;
    const sdtl_service_media_t *sdtl_media;

    static sdtl_service_t *
    sdtl_init(const char *service_mp, const char *service_name, size_t mtu,
              const sdtl_service_media_t *media) {
        sdtl_service_t *sdtl_service;
        sdtl_rv_t rv;

        rv = sdtl_service_init(&sdtl_service, service_name, service_mp, mtu, 4, media);
        REQUIRE(rv == SDTL_OK);

        sdtl_channel_cfg_t ch_cfg_rel = {
                .name = EQRB_SDTL_TEST_CHANEL_REL,
                .id = 1,
                .type = SDTL_CHANNEL_RELIABLE,
                .mtu_override = 0,
        };
        sdtl_channel_cfg_t ch_cfg_unrel = {
                .name = EQRB_SDTL_TEST_CHANEL_UNREL,
                .id = 2,
                .type = SDTL_CHANNEL_UNRELIABLE,
                .mtu_override = 0,
        };

        rv = sdtl_channel_create(sdtl_service, &ch_cfg_rel);
        REQUIRE(rv == SDTL_OK);

        rv = sdtl_channel_create(sdtl_service, &ch_cfg_unrel);
        REQUIRE(rv == SDTL_OK);

        return sdtl_service;
    }

    void sdtl_start() {
        sdtl_rv_t rv = sdtl_service_start(sdtl_service, media_path.c_str(), media_data_handle);
        REQUIRE(rv == SDTL_OK);
    }

    void sdtl_stop() {
        sdtl_rv_t rv = sdtl_service_stop(sdtl_service);
        REQUIRE(rv == SDTL_OK);
    }

    sdtl_service_t *init_environment() {
        auto service_bus_name = "sdtl_" + service_name + "_tst_bus";
        eswb_rv_t erv;

        do {
            erv = eswb_create(service_bus_name.c_str(), eswb_inter_thread, 100);
            if (erv == eswb_e_bus_exists) {
                eswb_rv_t erv2 = eswb_delete(service_bus_name.c_str());
                continue;
            } else {
                break;
            }
        } while (1);

        REQUIRE(erv == eswb_e_ok);

        sdtl_service_t *s_rv = sdtl_init(service_bus_name.c_str(),
                                          service_name.c_str(), 128, sdtl_media);

        return s_rv;
    }

public:
    sdtlBasic(const sdtl_service_media_t *sdtl_media_,
              const std::string &service_name_,
              const std::string &media_path_,
              void *media_h) : sdtl_media(sdtl_media_),
                                           service_name(service_name_),
                                           media_path(media_path_),
                                           media_data_handle(media_h) {

        sdtl_service = init_environment();
    }
};

class sdtlMemBridgeBasic : public sdtlBasic {

public:
        sdtlMemBridgeBasic(const std::string &service_name_,
                       const std::string &media_path_,
                       SDTLtestBridge &bridge) :
            sdtlBasic(&sdtl_test_media, service_name_, media_path_, &bridge) {
    }
};

class sdtlMemBridgeServer : public sdtlMemBridgeBasic {
    const std::string bus2replicate;
    uint32_t mask2replicate;

public:
    sdtlMemBridgeServer(const std::string &bus2replicate_, uint32_t mask2replicate_,
                        SDTLtestBridge &bridge) :
            sdtlMemBridgeBasic("server", "down", bridge),
            bus2replicate(bus2replicate_),
            mask2replicate(mask2replicate_){

    }

    void start() {
        sdtl_start();
//        eqrb_rv_t rv = eqrb_server_start(&server_handle, bus2replicate.c_str(), mask2replicate, NULL);
        eqrb_rv_t rv = eqrb_sdtl_server_start(
                "test_eqrb",
                service_name.c_str(),
                EQRB_SDTL_TEST_CHANEL_REL,
                EQRB_SDTL_TEST_CHANEL_UNREL,
                0xFFFFFFFF,
                bus2replicate.c_str(), NULL);

        REQUIRE(rv == eqrb_rv_ok);
    }

    void stop() {
        sdtl_stop();
//        eqrb_rv_t rv = eqrb_service_stop(&server_handle.h);
//        REQUIRE(rv == eqrb_rv_ok);
    }
};


class sdtlMemBridgeClient : public sdtlMemBridgeBasic {
    const std::string &replicate_to_path;

public:
    sdtlMemBridgeClient(const std::string &replicate_to_path_,
                        SDTLtestBridge &bridge ) :
            sdtlMemBridgeBasic("client", "up", bridge),
            replicate_to_path(replicate_to_path_){
    }

    void start() {
        sdtl_start();
        eqrb_rv_t rv = eqrb_sdtl_client_connect(service_name.c_str(), EQRB_SDTL_TEST_CHANEL_REL, EQRB_SDTL_TEST_CHANEL_UNREL,
                                                replicate_to_path.c_str(), 100);
        REQUIRE(rv == eqrb_rv_ok);
    }

    void stop() {
        sdtl_stop();

//        eqrb_rv_t rv = eqrb_service_stop(&client_handle.h);
//        REQUIRE(rv == eqrb_rv_ok);
    }
};

}

static eqrb_client_handle_t *repl_factory_ch;

/*
void repl_factory_tcp_init(std::string &src, std::string &dst) {
    eqrb_rv_t hrv;

    hrv = eqrb_tcp_server_start(3333);
    REQUIRE(hrv == eqrb_rv_ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    hrv = eqrb_tcp_client_create(&repl_factory_ch);
    REQUIRE(hrv == eqrb_rv_ok);

    char err_msg[EQRB_ERR_MSG_MAX_LEN + 1];
    hrv = eqrb_tcp_client_connect(repl_factory_ch, "127.0.0.1:3333", src.c_str(), dst.c_str(), 512, err_msg);
    if (hrv != eqrb_rv_ok) {
        FAIL("eqrb_tcp_client_connect failed " + std::to_string(hrv));
    }
}

void repl_factory_tcp_deinit() {
    eqrb_rv_t rv = eqrb_tcp_client_close(repl_factory_ch);
    REQUIRE(rv == eqrb_rv_ok);

    rv = eqrb_tcp_server_stop();
    REQUIRE(rv == eqrb_rv_ok);
}

void repl_factory_serial_init(std::string &src, std::string &dst) {
    eqrb_rv_t hrv;

    int fd1;
    int fd2;
#   define SERIAL1 "/tmp/vserial1"
#   define SERIAL2 "/tmp/vserial2"

    fd1 = open(SERIAL1, O_RDWR);
    fd2 = open(SERIAL2, O_RDWR);
    if (fd1 < 0 || fd2 < 0) {
        FAIL("Run virtual serial ports by socat using comment below");
    } else {
        close(fd1);
        close(fd2);
    }

     // socat -d -d pty,link=/tmp/vserial1,raw,echo=0 pty,link=/tmp/vserial2,raw,echo=0

    hrv = eqrb_serial_server_start(SERIAL1, 115200, src.c_str());
    REQUIRE(hrv == eqrb_rv_ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    hrv = eqrb_serial_client_connect(SERIAL2, 115200, dst.c_str(), 512);
    REQUIRE(hrv == eqrb_rv_ok);
}

void repl_factory_serial_deinit() {

}
*/

PseudoTopic *create_bus_and_arbitrary_hierarchy(eswb_type_t bus_type, const std::string &bus_name);

/*
TEST_CASE("EQRB bus state sync") {

    eswb_local_init(1);
    eswb_set_thread_name("main");

    eswb_rv_t erv;
    eqrb_rv_t rbrv;

    std::string src_bus_name = "src";
    auto src_bus = create_bus_and_arbitrary_hierarchy(eswb_inter_thread, src_bus_name);

    std::string dst_bus = "dst";
    std::string dst_bus_full_path = "itb:/" + dst_bus;
    erv = eswb_create(dst_bus.c_str(), eswb_inter_thread, 100);
    REQUIRE(erv == eswb_e_ok);

    eswb_topic_descr_t src_bus_td;

    erv = eswb_connect(src_bus->get_full_path().c_str(), &src_bus_td);
    REQUIRE(erv == eswb_e_ok);

    erv = eswb_event_queue_enable(src_bus_td, 40, 1024);
    REQUIRE(erv == eswb_e_ok);

    // going to recreate full src hierarhy inside dst bus
    erv = eswb_mkdir(dst_bus_full_path.c_str(), src_bus_name.c_str());
    REQUIRE(erv == eswb_e_ok);
    auto mounting_point = dst_bus_full_path + "/" + src_bus_name;
    eswb_topic_descr_t dst_bus_mp_td;
    erv = eswb_connect(mounting_point.c_str(), &dst_bus_mp_td);
    REQUIRE(erv == eswb_e_ok);


    // starting replication facilities
//    rbrv = eqrb_tcp_server_start(0);
    REQUIRE(rbrv == eqrb_rv_ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    eqrb_client_handle_t *client_handle;
//    rbrv = eqrb_tcp_client_create(&client_handle);
    REQUIRE(rbrv == eqrb_rv_ok);

    char err_msg[EQRB_ERR_MSG_MAX_LEN + 1];
//    rbrv = eqrb_tcp_client_connect(client_handle, "127.0.0.1", src_bus->get_full_path().c_str(), mounting_point.c_str(), 100, err_msg);
    if (rbrv != eqrb_rv_ok) {
        FAIL("eqrb_tcp_client_connect error " + std::to_string(rbrv));
    }

    eswb_topic_descr_t publisher_td;

    // wait for bus sync to complete
    eswb_topic_descr_t synced_folder_td;

    // path is chosen manually as the last in generated hierarchy
    erv = eswb_wait_connect_nested(dst_bus_mp_td, "ca/db", &synced_folder_td, 4000);
    CHECK(erv == eswb_e_ok);


    ExtractedTopicsRegistry reg;
    auto fakeRoot2compare = new PseudoTopic(src_bus_name);
    fakeRoot2compare->set_path_prefix(src_bus->get_path_prefix());
    reg.add2reg(fakeRoot2compare, 1);

    int extract_cnt = 0;
    eswb_topic_id_t next2tid = 0;
    eswb_topic_descr_t td;

    eswb_rv_t rv;
    rv = eswb_connect(mounting_point.c_str(), &td);

    do {
        topic_extract_t e;
        rv = eswb_get_next_topic_info(td, &next2tid, &e);
        if (rv == eswb_e_ok) {
            extract_cnt++;
            reg.add2reg(e.info.name, e.info.topic_id, e.parent_id);

//            std::cout << "Extracted topic \"" << e.info.name << "\" with id "
//            << e.info.topic_id << " with parent tid " << e.parent_id << std::endl;
        }
    } while (rv == eswb_e_ok);

    CHECK(extract_cnt == src_bus->subtopics_num() - 1);
    auto extracted_bus = reg.build_hierarhy();

    src_bus->print();
    extracted_bus->print();

    bool comparison = *src_bus == *extracted_bus;
    REQUIRE(comparison == true);

//    eqrb_tcp_server_stop();
//    eqrb_tcp_client_close(client_handle);
}
*/

TEST_CASE("EQBR - mem bridge with restart") {
    eswb_local_init(1);

    auto bus_from = "src";
    auto bus_to = "dst";
    auto bus_to2 = "dst2";

    SDTLtestBridge bridge;

    EqrbTestAgent::sdtlMemBridgeClient client(bus_to, bridge);
    EqrbTestAgent::sdtlMemBridgeServer server(bus_from, 0xFFFFFFFF, bridge);

    TestProcess process(server, bus_from);

    process.server_start();

    process.do_proclaim();
    std::this_thread::sleep_for(std::chrono::milliseconds (200));
    process.start_updater();

    replication_test_client(client, bus_to);

    client.stop();

    std::this_thread::sleep_for(std::chrono::milliseconds (200));

    EqrbTestAgent::sdtlMemBridgeClient client2(bus_to2, bridge);
    process.reset_counter();
    replication_test_client(client2, bus_to2);

    process.stop();
}


//TEST_CASE("EQBR - tcp", "[eqrb]") {
//    replication_test(repl_factory_tcp_init, repl_factory_tcp_deinit);
//}
//
//TEST_CASE("EQBR - serial", "[eqrb]") {
//    replication_test(repl_factory_serial_init, repl_factory_serial_deinit);
//}
