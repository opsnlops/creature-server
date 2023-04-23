// Generated by the gRPC C++ plugin.
// If you make any local change, they will be lost.
// source: server.proto

#include "server.pb.h"
#include "server.grpc.pb.h"

#include <functional>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/async_unary_call.h>
#include <grpcpp/impl/channel_interface.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/support/client_callback.h>
#include <grpcpp/support/message_allocator.h>
#include <grpcpp/support/method_handler.h>
#include <grpcpp/impl/rpc_service_method.h>
#include <grpcpp/support/server_callback.h>
#include <grpcpp/impl/server_callback_handlers.h>
#include <grpcpp/server_context.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/sync_stream.h>
namespace server {

static const char* CreatureServer_method_names[] = {
  "/server.CreatureServer/GetCreature",
  "/server.CreatureServer/GetAllCreatures",
  "/server.CreatureServer/CreateCreature",
  "/server.CreatureServer/UpdateCreature",
  "/server.CreatureServer/StreamLogs",
  "/server.CreatureServer/SearchCreatures",
  "/server.CreatureServer/ListCreatures",
  "/server.CreatureServer/StreamFrames",
  "/server.CreatureServer/GetServerStatus",
  "/server.CreatureServer/CreateAnimation",
  "/server.CreatureServer/ListAnimations",
};

std::unique_ptr< CreatureServer::Stub> CreatureServer::NewStub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options) {
  (void)options;
  std::unique_ptr< CreatureServer::Stub> stub(new CreatureServer::Stub(channel, options));
  return stub;
}

CreatureServer::Stub::Stub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options)
  : channel_(channel), rpcmethod_GetCreature_(CreatureServer_method_names[0], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_GetAllCreatures_(CreatureServer_method_names[1], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_CreateCreature_(CreatureServer_method_names[2], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_UpdateCreature_(CreatureServer_method_names[3], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_StreamLogs_(CreatureServer_method_names[4], options.suffix_for_stats(),::grpc::internal::RpcMethod::SERVER_STREAMING, channel)
  , rpcmethod_SearchCreatures_(CreatureServer_method_names[5], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_ListCreatures_(CreatureServer_method_names[6], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_StreamFrames_(CreatureServer_method_names[7], options.suffix_for_stats(),::grpc::internal::RpcMethod::CLIENT_STREAMING, channel)
  , rpcmethod_GetServerStatus_(CreatureServer_method_names[8], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_CreateAnimation_(CreatureServer_method_names[9], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_ListAnimations_(CreatureServer_method_names[10], options.suffix_for_stats(),::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  {}

::grpc::Status CreatureServer::Stub::GetCreature(::grpc::ClientContext* context, const ::server::CreatureId& request, ::server::Creature* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::CreatureId, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_GetCreature_, context, request, response);
}

void CreatureServer::Stub::async::GetCreature(::grpc::ClientContext* context, const ::server::CreatureId* request, ::server::Creature* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::CreatureId, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetCreature_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::GetCreature(::grpc::ClientContext* context, const ::server::CreatureId* request, ::server::Creature* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetCreature_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::Creature>* CreatureServer::Stub::PrepareAsyncGetCreatureRaw(::grpc::ClientContext* context, const ::server::CreatureId& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::Creature, ::server::CreatureId, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_GetCreature_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::Creature>* CreatureServer::Stub::AsyncGetCreatureRaw(::grpc::ClientContext* context, const ::server::CreatureId& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncGetCreatureRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::GetAllCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::server::GetAllCreaturesResponse* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::CreatureFilter, ::server::GetAllCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_GetAllCreatures_, context, request, response);
}

void CreatureServer::Stub::async::GetAllCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter* request, ::server::GetAllCreaturesResponse* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::CreatureFilter, ::server::GetAllCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetAllCreatures_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::GetAllCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter* request, ::server::GetAllCreaturesResponse* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetAllCreatures_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::GetAllCreaturesResponse>* CreatureServer::Stub::PrepareAsyncGetAllCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::GetAllCreaturesResponse, ::server::CreatureFilter, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_GetAllCreatures_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::GetAllCreaturesResponse>* CreatureServer::Stub::AsyncGetAllCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncGetAllCreaturesRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::CreateCreature(::grpc::ClientContext* context, const ::server::Creature& request, ::server::DatabaseInfo* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_CreateCreature_, context, request, response);
}

void CreatureServer::Stub::async::CreateCreature(::grpc::ClientContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_CreateCreature_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::CreateCreature(::grpc::ClientContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_CreateCreature_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::PrepareAsyncCreateCreatureRaw(::grpc::ClientContext* context, const ::server::Creature& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::DatabaseInfo, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_CreateCreature_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::AsyncCreateCreatureRaw(::grpc::ClientContext* context, const ::server::Creature& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncCreateCreatureRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::UpdateCreature(::grpc::ClientContext* context, const ::server::Creature& request, ::server::DatabaseInfo* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_UpdateCreature_, context, request, response);
}

void CreatureServer::Stub::async::UpdateCreature(::grpc::ClientContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_UpdateCreature_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::UpdateCreature(::grpc::ClientContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_UpdateCreature_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::PrepareAsyncUpdateCreatureRaw(::grpc::ClientContext* context, const ::server::Creature& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::DatabaseInfo, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_UpdateCreature_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::AsyncUpdateCreatureRaw(::grpc::ClientContext* context, const ::server::Creature& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncUpdateCreatureRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::ClientReader< ::server::LogItem>* CreatureServer::Stub::StreamLogsRaw(::grpc::ClientContext* context, const ::server::LogFilter& request) {
  return ::grpc::internal::ClientReaderFactory< ::server::LogItem>::Create(channel_.get(), rpcmethod_StreamLogs_, context, request);
}

void CreatureServer::Stub::async::StreamLogs(::grpc::ClientContext* context, const ::server::LogFilter* request, ::grpc::ClientReadReactor< ::server::LogItem>* reactor) {
  ::grpc::internal::ClientCallbackReaderFactory< ::server::LogItem>::Create(stub_->channel_.get(), stub_->rpcmethod_StreamLogs_, context, request, reactor);
}

::grpc::ClientAsyncReader< ::server::LogItem>* CreatureServer::Stub::AsyncStreamLogsRaw(::grpc::ClientContext* context, const ::server::LogFilter& request, ::grpc::CompletionQueue* cq, void* tag) {
  return ::grpc::internal::ClientAsyncReaderFactory< ::server::LogItem>::Create(channel_.get(), cq, rpcmethod_StreamLogs_, context, request, true, tag);
}

::grpc::ClientAsyncReader< ::server::LogItem>* CreatureServer::Stub::PrepareAsyncStreamLogsRaw(::grpc::ClientContext* context, const ::server::LogFilter& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncReaderFactory< ::server::LogItem>::Create(channel_.get(), cq, rpcmethod_StreamLogs_, context, request, false, nullptr);
}

::grpc::Status CreatureServer::Stub::SearchCreatures(::grpc::ClientContext* context, const ::server::CreatureName& request, ::server::Creature* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::CreatureName, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_SearchCreatures_, context, request, response);
}

void CreatureServer::Stub::async::SearchCreatures(::grpc::ClientContext* context, const ::server::CreatureName* request, ::server::Creature* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::CreatureName, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_SearchCreatures_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::SearchCreatures(::grpc::ClientContext* context, const ::server::CreatureName* request, ::server::Creature* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_SearchCreatures_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::Creature>* CreatureServer::Stub::PrepareAsyncSearchCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureName& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::Creature, ::server::CreatureName, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_SearchCreatures_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::Creature>* CreatureServer::Stub::AsyncSearchCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureName& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncSearchCreaturesRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::ListCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::server::ListCreaturesResponse* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::CreatureFilter, ::server::ListCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_ListCreatures_, context, request, response);
}

void CreatureServer::Stub::async::ListCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter* request, ::server::ListCreaturesResponse* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::CreatureFilter, ::server::ListCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_ListCreatures_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::ListCreatures(::grpc::ClientContext* context, const ::server::CreatureFilter* request, ::server::ListCreaturesResponse* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_ListCreatures_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::ListCreaturesResponse>* CreatureServer::Stub::PrepareAsyncListCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::ListCreaturesResponse, ::server::CreatureFilter, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_ListCreatures_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::ListCreaturesResponse>* CreatureServer::Stub::AsyncListCreaturesRaw(::grpc::ClientContext* context, const ::server::CreatureFilter& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncListCreaturesRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::ClientWriter< ::server::Frame>* CreatureServer::Stub::StreamFramesRaw(::grpc::ClientContext* context, ::server::FrameResponse* response) {
  return ::grpc::internal::ClientWriterFactory< ::server::Frame>::Create(channel_.get(), rpcmethod_StreamFrames_, context, response);
}

void CreatureServer::Stub::async::StreamFrames(::grpc::ClientContext* context, ::server::FrameResponse* response, ::grpc::ClientWriteReactor< ::server::Frame>* reactor) {
  ::grpc::internal::ClientCallbackWriterFactory< ::server::Frame>::Create(stub_->channel_.get(), stub_->rpcmethod_StreamFrames_, context, response, reactor);
}

::grpc::ClientAsyncWriter< ::server::Frame>* CreatureServer::Stub::AsyncStreamFramesRaw(::grpc::ClientContext* context, ::server::FrameResponse* response, ::grpc::CompletionQueue* cq, void* tag) {
  return ::grpc::internal::ClientAsyncWriterFactory< ::server::Frame>::Create(channel_.get(), cq, rpcmethod_StreamFrames_, context, response, true, tag);
}

::grpc::ClientAsyncWriter< ::server::Frame>* CreatureServer::Stub::PrepareAsyncStreamFramesRaw(::grpc::ClientContext* context, ::server::FrameResponse* response, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncWriterFactory< ::server::Frame>::Create(channel_.get(), cq, rpcmethod_StreamFrames_, context, response, false, nullptr);
}

::grpc::Status CreatureServer::Stub::GetServerStatus(::grpc::ClientContext* context, const ::google::protobuf::Empty& request, ::server::ServerStatus* response) {
  return ::grpc::internal::BlockingUnaryCall< ::google::protobuf::Empty, ::server::ServerStatus, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_GetServerStatus_, context, request, response);
}

void CreatureServer::Stub::async::GetServerStatus(::grpc::ClientContext* context, const ::google::protobuf::Empty* request, ::server::ServerStatus* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::google::protobuf::Empty, ::server::ServerStatus, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetServerStatus_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::GetServerStatus(::grpc::ClientContext* context, const ::google::protobuf::Empty* request, ::server::ServerStatus* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_GetServerStatus_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::ServerStatus>* CreatureServer::Stub::PrepareAsyncGetServerStatusRaw(::grpc::ClientContext* context, const ::google::protobuf::Empty& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::ServerStatus, ::google::protobuf::Empty, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_GetServerStatus_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::ServerStatus>* CreatureServer::Stub::AsyncGetServerStatusRaw(::grpc::ClientContext* context, const ::google::protobuf::Empty& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncGetServerStatusRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::CreateAnimation(::grpc::ClientContext* context, const ::server::Animation& request, ::server::DatabaseInfo* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::Animation, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_CreateAnimation_, context, request, response);
}

void CreatureServer::Stub::async::CreateAnimation(::grpc::ClientContext* context, const ::server::Animation* request, ::server::DatabaseInfo* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::Animation, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_CreateAnimation_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::CreateAnimation(::grpc::ClientContext* context, const ::server::Animation* request, ::server::DatabaseInfo* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_CreateAnimation_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::PrepareAsyncCreateAnimationRaw(::grpc::ClientContext* context, const ::server::Animation& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::DatabaseInfo, ::server::Animation, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_CreateAnimation_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::DatabaseInfo>* CreatureServer::Stub::AsyncCreateAnimationRaw(::grpc::ClientContext* context, const ::server::Animation& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncCreateAnimationRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status CreatureServer::Stub::ListAnimations(::grpc::ClientContext* context, const ::server::AnimationFilter& request, ::server::ListAnimationsResponse* response) {
  return ::grpc::internal::BlockingUnaryCall< ::server::AnimationFilter, ::server::ListAnimationsResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), rpcmethod_ListAnimations_, context, request, response);
}

void CreatureServer::Stub::async::ListAnimations(::grpc::ClientContext* context, const ::server::AnimationFilter* request, ::server::ListAnimationsResponse* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall< ::server::AnimationFilter, ::server::ListAnimationsResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_ListAnimations_, context, request, response, std::move(f));
}

void CreatureServer::Stub::async::ListAnimations(::grpc::ClientContext* context, const ::server::AnimationFilter* request, ::server::ListAnimationsResponse* response, ::grpc::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create< ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(stub_->channel_.get(), stub_->rpcmethod_ListAnimations_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::server::ListAnimationsResponse>* CreatureServer::Stub::PrepareAsyncListAnimationsRaw(::grpc::ClientContext* context, const ::server::AnimationFilter& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderHelper::Create< ::server::ListAnimationsResponse, ::server::AnimationFilter, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(channel_.get(), cq, rpcmethod_ListAnimations_, context, request);
}

::grpc::ClientAsyncResponseReader< ::server::ListAnimationsResponse>* CreatureServer::Stub::AsyncListAnimationsRaw(::grpc::ClientContext* context, const ::server::AnimationFilter& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsyncListAnimationsRaw(context, request, cq);
  result->StartCall();
  return result;
}

CreatureServer::Service::Service() {
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[0],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::CreatureId, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::CreatureId* req,
             ::server::Creature* resp) {
               return service->GetCreature(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[1],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::CreatureFilter, ::server::GetAllCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::CreatureFilter* req,
             ::server::GetAllCreaturesResponse* resp) {
               return service->GetAllCreatures(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[2],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::Creature* req,
             ::server::DatabaseInfo* resp) {
               return service->CreateCreature(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[3],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::Creature, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::Creature* req,
             ::server::DatabaseInfo* resp) {
               return service->UpdateCreature(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[4],
      ::grpc::internal::RpcMethod::SERVER_STREAMING,
      new ::grpc::internal::ServerStreamingHandler< CreatureServer::Service, ::server::LogFilter, ::server::LogItem>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::LogFilter* req,
             ::grpc::ServerWriter<::server::LogItem>* writer) {
               return service->StreamLogs(ctx, req, writer);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[5],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::CreatureName, ::server::Creature, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::CreatureName* req,
             ::server::Creature* resp) {
               return service->SearchCreatures(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[6],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::CreatureFilter, ::server::ListCreaturesResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::CreatureFilter* req,
             ::server::ListCreaturesResponse* resp) {
               return service->ListCreatures(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[7],
      ::grpc::internal::RpcMethod::CLIENT_STREAMING,
      new ::grpc::internal::ClientStreamingHandler< CreatureServer::Service, ::server::Frame, ::server::FrameResponse>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             ::grpc::ServerReader<::server::Frame>* reader,
             ::server::FrameResponse* resp) {
               return service->StreamFrames(ctx, reader, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[8],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::google::protobuf::Empty, ::server::ServerStatus, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::google::protobuf::Empty* req,
             ::server::ServerStatus* resp) {
               return service->GetServerStatus(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[9],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::Animation, ::server::DatabaseInfo, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::Animation* req,
             ::server::DatabaseInfo* resp) {
               return service->CreateAnimation(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      CreatureServer_method_names[10],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< CreatureServer::Service, ::server::AnimationFilter, ::server::ListAnimationsResponse, ::grpc::protobuf::MessageLite, ::grpc::protobuf::MessageLite>(
          [](CreatureServer::Service* service,
             ::grpc::ServerContext* ctx,
             const ::server::AnimationFilter* req,
             ::server::ListAnimationsResponse* resp) {
               return service->ListAnimations(ctx, req, resp);
             }, this)));
}

CreatureServer::Service::~Service() {
}

::grpc::Status CreatureServer::Service::GetCreature(::grpc::ServerContext* context, const ::server::CreatureId* request, ::server::Creature* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::GetAllCreatures(::grpc::ServerContext* context, const ::server::CreatureFilter* request, ::server::GetAllCreaturesResponse* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::CreateCreature(::grpc::ServerContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::UpdateCreature(::grpc::ServerContext* context, const ::server::Creature* request, ::server::DatabaseInfo* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::StreamLogs(::grpc::ServerContext* context, const ::server::LogFilter* request, ::grpc::ServerWriter< ::server::LogItem>* writer) {
  (void) context;
  (void) request;
  (void) writer;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::SearchCreatures(::grpc::ServerContext* context, const ::server::CreatureName* request, ::server::Creature* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::ListCreatures(::grpc::ServerContext* context, const ::server::CreatureFilter* request, ::server::ListCreaturesResponse* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::StreamFrames(::grpc::ServerContext* context, ::grpc::ServerReader< ::server::Frame>* reader, ::server::FrameResponse* response) {
  (void) context;
  (void) reader;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::GetServerStatus(::grpc::ServerContext* context, const ::google::protobuf::Empty* request, ::server::ServerStatus* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::CreateAnimation(::grpc::ServerContext* context, const ::server::Animation* request, ::server::DatabaseInfo* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status CreatureServer::Service::ListAnimations(::grpc::ServerContext* context, const ::server::AnimationFilter* request, ::server::ListAnimationsResponse* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}


}  // namespace server

