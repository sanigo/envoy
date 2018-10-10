#include "common/common/thread.h"

#include "extensions/transport_sockets/alts/config.h"

#include "test/core/tsi/alts/fake_handshaker/fake_handshaker_server.h"
#include "test/integration/http_integration.h"
#include "test/integration/integration.h"
#include "test/integration/server.h"
#include "test/integration/utility.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/utility.h"

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/impl/codegen/service_type.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Alts {

class AltsIntegrationTestBase : public HttpIntegrationTest,
                                public testing::TestWithParam<Network::Address::IpVersion> {
public:
  AltsIntegrationTestBase(const std::string& server_peer_identity,
                          const std::string& client_peer_identity)
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam(), realTime()),
        server_peer_identity_(server_peer_identity), client_peer_identity_(client_peer_identity) {}

  void initialize() override {
    config_helper_.addConfigModifier([this](envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
      auto transport_socket = bootstrap.mutable_static_resources()
                                  ->mutable_listeners(0)
                                  ->mutable_filter_chains(0)
                                  ->mutable_transport_socket();
      std::string yaml = R"EOF(
    name: envoy.transport_sockets.alts
    config:
      peer_service_accounts: []
      handshaker_service: ")EOF" +
                         fakeHandshakerServerAddress() + "\"";
      if (!server_peer_identity_.empty()) {
        yaml.replace(yaml.find("[]"), std::string::size_type(2), server_peer_identity_);
      }

      MessageUtil::loadFromYaml(TestEnvironment::substitute(yaml), *transport_socket);
    });
    HttpIntegrationTest::initialize();
    registerTestServerPorts({"http"});
  }

  void SetUp() override {
    fake_handshaker_server_thread_ = std::make_unique<Thread::Thread>([this]() {
      std::unique_ptr<grpc::Service> service = grpc::gcp::CreateFakeHandshakerService();

      std::string server_address = Network::Test::getLoopbackAddressUrlString(version_) + ":0";
      grpc::ServerBuilder builder;
      builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(),
                               &fake_handshaker_server_port_);
      builder.RegisterService(service.get());

      fake_handshaker_server_ = builder.BuildAndStart();
      fake_handshaker_server_ci_.setReady();
      fake_handshaker_server_->Wait();
    });

    fake_handshaker_server_ci_.waitReady();

    std::string client_yaml = R"EOF(
      peer_service_accounts: []
      handshaker_service: ")EOF" +
                              fakeHandshakerServerAddress() + "\"";
    if (!client_peer_identity_.empty()) {
      client_yaml.replace(client_yaml.find("[]"), std::string::size_type(2), client_peer_identity_);
    }

    NiceMock<Server::Configuration::MockTransportSocketFactoryContext> mock_factory_ctx;
    UpstreamAltsTransportSocketConfigFactory factory;

    ProtobufTypes::MessagePtr config = factory.createEmptyConfigProto();
    MessageUtil::loadFromYaml(TestEnvironment::substitute(client_yaml), *config);

    ENVOY_LOG_MISC(info, "{}", config->DebugString());

    client_alts_ = factory.createTransportSocketFactory(*config, mock_factory_ctx);
  }

  void TearDown() override {
    HttpIntegrationTest::cleanupUpstreamAndDownstream();
    dispatcher_->clearDeferredDeleteList();
    if (fake_handshaker_server_ != nullptr) {
      fake_handshaker_server_->Shutdown();
    }
    fake_handshaker_server_thread_->join();
  }

  Network::ClientConnectionPtr makeAltsConnection() {
    Network::Address::InstanceConstSharedPtr address = getAddress(version_, lookupPort("http"));
    return dispatcher_->createClientConnection(address, Network::Address::InstanceConstSharedPtr(),
                                               client_alts_->createTransportSocket(), nullptr);
  }

  std::string fakeHandshakerServerAddress() {
    return absl::StrCat(Network::Test::getLoopbackAddressUrlString(version_), ":",
                        std::to_string(fake_handshaker_server_port_));
  }

  Network::Address::InstanceConstSharedPtr getAddress(const Network::Address::IpVersion& version,
                                                      int port) {
    std::string url =
        "tcp://" + Network::Test::getLoopbackAddressUrlString(version) + ":" + std::to_string(port);
    return Network::Utility::resolveUrl(url);
  }

  const std::string server_peer_identity_;
  const std::string client_peer_identity_;
  Thread::ThreadPtr fake_handshaker_server_thread_;
  std::unique_ptr<grpc::Server> fake_handshaker_server_;
  ConditionalInitializer fake_handshaker_server_ci_;
  int fake_handshaker_server_port_{};
  Network::TransportSocketFactoryPtr client_alts_;
};

class AltsIntegrationTestValidPeer : public AltsIntegrationTestBase {
public:
  // FakeHandshake server sends peer_identity as peer service account. Set this
  // information into config to pass validation.
  AltsIntegrationTestValidPeer() : AltsIntegrationTestBase("[peer_identity]", "") {}
};

INSTANTIATE_TEST_CASE_P(IpVersions, AltsIntegrationTestValidPeer,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                        TestUtility::ipTestParamsToString);

// Verifies that when received peer service account passes validation, the alts
// handshake succeeds.
TEST_P(AltsIntegrationTestValidPeer, RouterRequestAndResponseWithBodyNoBuffer) {
  ConnectionCreationFunction creator = [this]() -> Network::ClientConnectionPtr {
    return makeAltsConnection();
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
}

class AltsIntegrationTestEmptyPeer : public AltsIntegrationTestBase {
public:
  AltsIntegrationTestEmptyPeer() : AltsIntegrationTestBase("", "") {}
};

INSTANTIATE_TEST_CASE_P(IpVersions, AltsIntegrationTestEmptyPeer,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                        TestUtility::ipTestParamsToString);

// Verifies that when peer service account is not set into config, the alts
// handshake succeeds.
TEST_P(AltsIntegrationTestEmptyPeer, RouterRequestAndResponseWithBodyNoBuffer) {
  ConnectionCreationFunction creator = [this]() -> Network::ClientConnectionPtr {
    return makeAltsConnection();
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
}

class AltsIntegrationTestClientInvalidPeer : public AltsIntegrationTestBase {
public:
  AltsIntegrationTestClientInvalidPeer()
      : AltsIntegrationTestBase("", "invalid_client_identity") {}
};

INSTANTIATE_TEST_CASE_P(IpVersions, AltsIntegrationTestClientInvalidPeer,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                        TestUtility::ipTestParamsToString);

// Verifies that when client receives peer service account which does not match
// any accounts in config, the handshake will fail and client closes connection.
TEST_P(AltsIntegrationTestClientInvalidPeer, clientValidationFail) {
  initialize();
  codec_client_ = makeRawHttpConnection(makeAltsConnection());
  EXPECT_FALSE(codec_client_->connected());
}

class AltsIntegrationTestServerInvalidPeer : public AltsIntegrationTestBase {
public:
  AltsIntegrationTestServerInvalidPeer()
      : AltsIntegrationTestBase("invalid_server_identity", "") {}
};

INSTANTIATE_TEST_CASE_P(IpVersions, AltsIntegrationTestServerInvalidPeer,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                        TestUtility::ipTestParamsToString);

// Verifies that when Envoy receives peer service account which does not match
// any accounts in config, the handshake will fail and Envoy closes connection.
TEST_P(AltsIntegrationTestServerInvalidPeer, ServerValidationFail) {
  initialize();

  testing::NiceMock<Network::MockConnectionCallbacks> client_callbacks;
  Network::ClientConnectionPtr client_conn = makeAltsConnection();
  client_conn->addConnectionCallbacks(client_callbacks);
  EXPECT_CALL(client_callbacks,
            onEvent(Network::ConnectionEvent::Connected));
  client_conn->connect();

  EXPECT_CALL(client_callbacks, onEvent(Network::ConnectionEvent::RemoteClose))
      .WillOnce(Invoke(
          [&](Network::ConnectionEvent) -> void { dispatcher_->exit(); }));
  dispatcher_->run(Event::Dispatcher::RunType::Block);
}

} // namespace Alts
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
