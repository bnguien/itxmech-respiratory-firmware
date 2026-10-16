"""Run the actual state-machine implementation against deterministic adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCKS = r"""
#pragma once
#include <cassert>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>
struct String : std::string {
 using std::string::string;
 bool isEmpty() const { return empty(); }
};
struct RecordingJob { String messageId, visitId, recordingId, phase; };
inline RecordingJob saved;
inline bool pending=false, fileExists=false, finalized=false, captureFails=false;
inline int captureCalls=0, putCalls=0, urlCalls=0, completeCalls=0;
inline std::deque<int> urls, putCodes, completes;
inline std::set<String> messages, visits;
inline std::vector<std::string> events;
inline int response(std::deque<int> &codes) {
 if (codes.empty()) return 200;
 int code=codes.front(); codes.pop_front(); return code;
}
inline void delay(unsigned) {}
inline unsigned esp_random() {return 0;}
constexpr uint32_t kSampleRate=16000;
struct Logger {
 void println(const char *) {}
 template<class... T> void printf(const char *, T...) {}
};
inline Logger Serial;
struct File {
 explicit operator bool() const {return fileExists;}
 void close() {}
};
struct RecordingStorage {
 bool begin() {return true;}
 bool seen(const String &m,const String &v) {return messages.count(m)||visits.count(v);}
 bool hasRoom(uint32_t) {return true;}
 bool save(const RecordingJob &j) {saved=j; pending=true; return true;}
 bool accept(const RecordingJob &j) {
   messages.insert(j.messageId); visits.insert(j.visitId); return save(j);
 }
 bool load(RecordingJob &j) {j=saved; return pending;}
 File create() {fileExists=true; return {};}
 File open() {return {};}
 bool validRecording(uint32_t bytes) {return fileExists && finalized && bytes==640000;}
 bool finish() {assert(saved.phase=="done"); fileExists=false; pending=false; return true;}
};
struct AudioRecorder {
 bool begin() {return true;}
 bool record(File &,uint32_t) {
   ++captureCalls; finalized=!captureFails; return finalized;
 }
};
struct UploadTicket {};
struct RecordingApiClient {
 static bool success(int c) {return c>=200&&c<300;}
 static bool retryable(int c) {return c<=0||c==408||c==429||c>=500;}
 int requestUrl(RecordingJob &j, UploadTicket &) {
   assert(finalized); ++urlCalls; j.recordingId="backend-id"; return response(urls);
 }
 int upload(const UploadTicket &, File &) {
   assert(saved.phase=="uploading" && saved.recordingId=="backend-id");
   ++putCalls; return response(putCodes);
 }
 int complete(const RecordingJob &) {
   assert(saved.phase=="completing"); ++completeCalls; return response(completes);
 }
};
enum class DeviceState {BOOT,IDLE,VISIT_ASSIGNED,RECORDING,RECORDING_COMPLETE,
 REQUESTING_UPLOAD_URL,UPLOADING,COMPLETING,ERROR,RETRY};
using EventHandler=void (*)(const char *,const String &);
class DeviceFlow {
 public:
 explicit DeviceFlow(EventHandler h):emit(h) {}
 bool begin(); bool resume(); bool assign(const String &,const String &);
 private:
 RecordingStorage storage; AudioRecorder audio; RecordingApiClient api;
 RecordingJob job; DeviceState state=DeviceState::BOOT; EventHandler emit;
 bool run(); bool checkpoint(const char *); bool fail(int);
 bool upload(); bool complete(); void backoff(unsigned);
};
"""
CASES = r"""
#include "DeviceFlow.cpp"
void event(const char *type,const String &visit) {
 assert(visit=="visit");
 if (std::string(type)=="UPLOAD_SUCCESS") assert(saved.phase=="done");
 events.push_back(type);
}
void reset() {
 saved={}; pending=fileExists=finalized=captureFails=false;
 captureCalls=putCalls=urlCalls=completeCalls=0;
 urls.clear(); putCodes.clear(); completes.clear(); messages.clear(); visits.clear(); events.clear();
}
void restored(const char *phase) {
 reset(); saved={"msg","visit","backend-id",phase};
 pending=fileExists=finalized=true;
}
int main() {
 reset();
 DeviceFlow normal(event); assert(normal.begin() && normal.resume());
 assert(normal.assign("msg","visit"));
 assert((events==std::vector<std::string>{"VISIT_ASSIGNED","RECORDING_STARTED",
   "RECORDING_STOPPED","UPLOAD_STARTED","UPLOAD_SUCCESS"}));
 assert(captureCalls==1 && putCalls==1 && completeCalls==1 && !fileExists && !pending);
 assert(normal.assign("msg","visit") && captureCalls==1);
 assert(normal.assign("different-message","visit") && captureCalls==1);

 reset(); completes={-1,503,200};
 DeviceFlow lostComplete(event); assert(lostComplete.assign("msg","visit"));
 assert(putCalls==1 && urlCalls==1 && completeCalls==3 && captureCalls==1);

 reset(); putCodes={403,200};
 DeviceFlow expired(event); assert(expired.assign("msg","visit"));
 assert(urlCalls==2 && putCalls==2 && captureCalls==1);

 for (int code:{401,404,409,413,415}) {
   reset(); urls={code};
   DeviceFlow rejected(event); assert(!rejected.assign("msg","visit"));
   assert(urlCalls==1 && putCalls==0 && completeCalls==0 && fileExists);
   assert(saved.phase=="error" && events.back()=="UPLOAD_FAILED");
 }
 reset(); completes={503,503,503,503,200};
 DeviceFlow exhausted(event); assert(!exhausted.assign("msg","visit"));
 assert(completeCalls==4 && fileExists && saved.phase=="error");

 restored("completing");
 DeviceFlow rebootComplete(event); assert(rebootComplete.resume());
 assert(captureCalls==0 && putCalls==0 && urlCalls==0 && completeCalls==1);

 restored("uploading");
 DeviceFlow rebootPut(event); assert(rebootPut.resume());
 assert(captureCalls==0 && urlCalls==1 && putCalls==1 && completeCalls==1);

 restored("recorded"); finalized=false;
 DeviceFlow brokenWav(event); assert(!brokenWav.resume());
 assert(urlCalls==0 && putCalls==0 && completeCalls==0 && fileExists);
 assert(saved.phase=="error" && events.back()=="UPLOAD_FAILED");

 restored("recording");
 DeviceFlow interrupted(event); assert(!interrupted.resume());
 assert(captureCalls==0 && putCalls==0 && fileExists && saved.phase=="error");

 reset(); captureFails=true;
 DeviceFlow badAudio(event); assert(!badAudio.assign("msg","visit"));
 assert(fileExists && putCalls==0 && urlCalls==0 && completeCalls==0);
}
"""

with tempfile.TemporaryDirectory(prefix="respicare-flow-",
                                 dir=os.environ.get("RESPICARE_TEST_TMPDIR")) as tmp:
    path = Path(tmp)
    # Copy the unchanged implementation so its quoted include selects fake adapters.
    (path / "DeviceFlow.cpp").write_bytes((ROOT / "src/DeviceFlow.cpp").read_bytes())
    (path / "DeviceFlow.h").write_text(MOCKS)
    (path / "BuildConfig.h").write_text("#define RECORD_SECONDS 20\n")
    (path / "WiFi.h").write_text("")
    (path / "test.cpp").write_text(CASES)
    binary = path / "flow-test"
    subprocess.run(["c++", "-std=c++17", "-I", str(path), str(path / "test.cpp"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: event order, dedup, expiry, bounded retries, API errors, reboot checkpoints, capture failure")
