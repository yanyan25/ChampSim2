#include <catch.hpp>

#include "../../../branch/bimodal/bimodal.h"

TEST_CASE("The bimodal predictor predicts taken after many taken branches")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  }

  REQUIRE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}

TEST_CASE("The bimodal predictor predicts not taken after many not-taken branches")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  }

  REQUIRE_FALSE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}

TEST_CASE("After saturating not-taken, the bimodal predictor continues to predict not taken after one taken branch")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  }

  uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  REQUIRE_FALSE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}

TEST_CASE("After saturating not-taken, the bimodal predictor predicts taken after two taken branches")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  }

  uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  REQUIRE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}

TEST_CASE("After saturating taken, the bimodal predictor continues to predict taken after one not-taken branch")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  }

  uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  REQUIRE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}

TEST_CASE("After saturating taken, the bimodal predictor predicts not taken after two not-taken branches")
{
  bimodal uut{nullptr};
  champsim::address ip_under_test{0xdeadbeef};

  for (std::size_t i{0}; i < 100; ++i) {
    uut.last_branch_result(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{});
  }

  uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  uut.last_branch_result(ip_under_test, champsim::address{}, false, 0, champsim::address_space_id{});
  REQUIRE_FALSE(uut.predict_branch(ip_under_test, champsim::address{}, true, 0, champsim::address_space_id{}));
}
