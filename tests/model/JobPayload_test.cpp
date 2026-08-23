#include <gtest/gtest.h>

#include "api/WebSocketEnvelope.h"
#include "server/jobs/JobPayload.h"
#include "server/ws/dto/websocket/MessageTypes.h"

namespace creatures {

TEST(JobPayloadJson, PreservesJobProgressWireShape) {
    jobs::JobState state{"11111111-1111-4111-8111-111111111111", jobs::JobType::LipSync, "audio.wav"};
    state.status = jobs::JobStatus::Running;
    state.progress = 0.75f;

    EXPECT_EQ(jobs::jobProgressToJson(state), (nlohmann::json{{"job_id", state.jobId},
                                                              {"job_type", "lip-sync"},
                                                              {"status", "running"},
                                                              {"progress", 0.75f},
                                                              {"details", "audio.wav"}}));
    EXPECT_EQ(api::webSocketEnvelopeToJson(ws::toString(ws::MessageType::JobProgress), jobs::jobProgressToJson(state)),
              (nlohmann::json{{"command", "job-progress"},
                              {"payload",
                               {{"job_id", state.jobId},
                                {"job_type", "lip-sync"},
                                {"status", "running"},
                                {"progress", 0.75f},
                                {"details", "audio.wav"}}}}));
}

TEST(JobPayloadJson, PreservesJobCompletionWireShape) {
    jobs::JobState state{"22222222-2222-4222-8222-222222222222", jobs::JobType::Dialog, "scene.json"};
    state.status = jobs::JobStatus::Completed;
    state.result = "{\"sound_id\":\"33333333-3333-4333-8333-333333333333\"}";

    EXPECT_EQ(jobs::jobCompleteToJson(state), (nlohmann::json{{"job_id", state.jobId},
                                                              {"job_type", "dialog"},
                                                              {"status", "completed"},
                                                              {"result", state.result},
                                                              {"details", "scene.json"}}));
    EXPECT_EQ(api::webSocketEnvelopeToJson(ws::toString(ws::MessageType::JobComplete), jobs::jobCompleteToJson(state)),
              (nlohmann::json{{"command", "job-complete"},
                              {"payload",
                               {{"job_id", state.jobId},
                                {"job_type", "dialog"},
                                {"status", "completed"},
                                {"result", state.result},
                                {"details", "scene.json"}}}}));
}

} // namespace creatures
