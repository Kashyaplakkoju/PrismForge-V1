#include "windows/src/effects/packs/pack_manager.h"
#include "windows/src/effects/packs/sha256.h"
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <wx/init.h>
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>
#include <algorithm>
#include "windows/src/effects/packs/pack_transaction_store.h"
using namespace PrismForge::Effects::Packs;
namespace fs=std::filesystem;
static int checks=0,groups=0,failures=0;
void Check(bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);}
void Write(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(s.data(),s.size());f.close();Check(!f.fail(),"fixture write failed");}
std::string Read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
const std::string oldBytes="LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
const std::string newBytes="# NEW\n"+oldBytes;
PackManifest Fixture(const fs::path& p,const std::string& bytes=oldBytes){
    PackManifest m;m.packId="com.prismforge.transactions";m.version="1.0.0";m.displayName="Transaction test";m.previewImagePath="images/preview.raw";
    const std::string raw(16384,char(0x80));
    m.assets={{PackAssetType::CubeLut,"look","look.cube",Sha256::ComputeHex((const uint8_t*)bytes.data(),bytes.size()),uint64_t(bytes.size())},
        {PackAssetType::PreviewImage,"preview",m.previewImagePath,Sha256::ComputeHex((const uint8_t*)raw.data(),raw.size()),uint64_t(raw.size())}};
    Write(p/"look.cube",bytes);Write(p/m.previewImagePath,raw);std::string error;
    Check(ManifestJson::WriteToFile((p/"manifest.json").u8string(),m,error),"fixture manifest: "+error);return m;
}
fs::path Target(const fs::path& lib){return lib/"com.prismforge.transactions"/"1.0.0";}
void Exact(const fs::path& lib,const std::string& expected){std::string e;auto p=PackManager::LoadPack(Target(lib).u8string(),e);
    Check(bool(p),"load expected version: "+e);auto b=p->ReadAssetBytes("look");Check(std::string(b.begin(),b.end())==expected,"wrong exact look bytes");
    Check(p->GetPreviewBytes()==std::vector<uint8_t>(16384,0x80),"wrong exact preview bytes");}
std::wstring W(const std::string& s){return fs::path(s).wstring();}
std::wstring Quote(const std::string& s){return L"\""+W(s)+L"\"";}
struct Child{HANDLE process=nullptr;DWORD pid=0;Child()=default;Child(const Child&)=delete;Child(Child&& a):process(a.process),pid(a.pid){a.process=nullptr;}
    ~Child(){if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT){TerminateProcess(process,125);WaitForSingleObject(process,5000);}CloseHandle(process);}}
    DWORD Exit(DWORD timeout=20000){Check(WaitForSingleObject(process,timeout)==WAIT_OBJECT_0,"child deadline");DWORD code;Check(GetExitCodeProcess(process,&code)!=0,"child exit retrieval");return code;}
    DWORD Kill(){Check(TerminateProcess(process,77)!=0,"kill publisher");return Exit(5000);}
};
Child Spawn(const std::wstring& args){wchar_t self[32768];Check(GetModuleFileNameW(nullptr,self,32768)>0,"self exe");std::wstring c=L"\""+std::wstring(self)+L"\" "+args;
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};Check(CreateProcessW(self,c.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=0,"spawn child");
    CloseHandle(pi.hThread);Child child;child.process=pi.hProcess;child.pid=pi.dwProcessId;return child;}
struct Events{HANDLE ready,release;std::string a,b;Events(){static int n=0;std::string base="Local\\PFS_Transactions_"+std::to_string(GetCurrentProcessId())+"_"+std::to_string(++n);a=base+"_ready";b=base+"_release";
    ready=CreateEventW(nullptr,TRUE,FALSE,W(a).c_str());release=CreateEventW(nullptr,TRUE,FALSE,W(b).c_str());Check(ready&&release,"events");}
    ~Events(){CloseHandle(ready);CloseHandle(release);}void Wait(){Check(WaitForSingleObject(ready,15000)==WAIT_OBJECT_0,"checkpoint ready deadline");}void Release(){Check(SetEvent(release)!=0,"release event");}};
struct Hook{PackTransactionPhase phase;HANDLE ready,release;};
void HookCall(PackTransactionPhase phase,void* v){auto& h=*(Hook*)v;if(phase==h.phase){SetEvent(h.ready);if(WaitForSingleObject(h.release,60000)!=WAIT_OBJECT_0)ExitProcess(124);}}
std::wstring PublishArgs(const fs::path& source,const fs::path& lib,int phase,const Events& ev){return L"--publish "+Quote(source.u8string())+L" "+Quote(lib.u8string())+L" "+std::to_wstring(phase)+L" "+Quote(ev.a)+L" "+Quote(ev.b);}
std::string MutexName(const fs::path& p){auto s=PackStorage::PathIdentity(p.lexically_normal());return "Local\\PrismForge_PackLibrary_Lock_"+Sha256::ComputeHex((const uint8_t*)s.data(),s.size());}

bool Junction(const fs::path& link,const fs::path& target){
    fs::create_directories(link);HANDLE h=CreateFileW(PackStorage::NativePath(link).c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    std::wstring sub=L"\\??\\"+fs::absolute(target).native(),print=fs::absolute(target).native();
    std::vector<uint8_t>b(16+(sub.size()+print.size()+2)*2,0);*(DWORD*)b.data()=IO_REPARSE_TAG_MOUNT_POINT;auto f=(WORD*)(b.data()+4);
    f[0]=WORD(b.size()-8);f[1]=0;f[2]=0;f[3]=WORD(sub.size()*2);f[4]=WORD((sub.size()+1)*2);f[5]=WORD(print.size()*2);
    std::memcpy(b.data()+16,sub.data(),sub.size()*2);std::memcpy(b.data()+16+(sub.size()+1)*2,print.data(),print.size()*2);
    DWORD got;bool ok=DeviceIoControl(h,FSCTL_SET_REPARSE_POINT,b.data(),DWORD(b.size()),nullptr,0,&got,nullptr)!=0;CloseHandle(h);return ok;
}
void Group(const char* name,const std::function<void()>& test){++groups;try{PackManager::SetFaultSeam(PackFaultSeam::None);test();std::cout<<"PASS "<<name<<"\n";}catch(const std::exception& e){++failures;std::cout<<"FAIL "<<name<<": "<<e.what()<<"\n";}PackManager::SetFaultSeam(PackFaultSeam::None);}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--exit-proof")return 42;
    if(argc==2&&std::string(argv[1])=="--hang-proof"){Sleep(INFINITE);return 99;}
    if(argc>1&&std::string(argv[1])=="--publish"){
        Hook h{PackTransactionPhase(std::stoi(argv[4])),OpenEventW(EVENT_MODIFY_STATE,FALSE,W(argv[5]).c_str()),OpenEventW(SYNCHRONIZE,FALSE,W(argv[6]).c_str())};
        if(!h.ready||!h.release)return 90;PackManager::SetTransactionCheckpoint(HookCall,&h);std::string e;bool ok=PackManager::InstallPack(argv[2],argv[3],e);if(!ok)std::cerr<<e<<"\n";return ok?0:1;
    }
    if(argc>1&&std::string(argv[1])=="--recover") {std::string e;bool ok=PackManager::RecoverInterruptedTransactions(argv[2],e);
        if(argc>3)Write(argv[3],std::to_string(PackManager::GetLastProcessLockResult())+"\n"+e);return ok?0:1;}
    if(argc>1&&std::string(argv[1])=="--contend") {PackManager::SetLockTimeoutForTests(uint32_t(std::stoi(argv[3])));std::string e;
        bool ok=PackManager::UninstallPack(argv[2],"com.prismforge.transactions","1.0.0",e);
        Write(argv[4],std::to_string(PackManager::GetLastProcessLockResult())+"\n"+e);return ok?0:1;}
    if(argc>1&&std::string(argv[1])=="--reader") {
        std::string e;auto pack=PackManager::LoadPack(Target(argv[2]).u8string(),e);if(!pack)return 80;
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,W(argv[3]).c_str()),stop=OpenEventW(SYNCHRONIZE,FALSE,W(argv[4]).c_str());
        if(!ready||!stop)return 81;SetEvent(ready);uint64_t reads=0;
        while(WaitForSingleObject(stop,0)==WAIT_TIMEOUT){auto b=pack->ReadAssetBytes("look");if(std::string(b.begin(),b.end())!=oldBytes||pack->GetPreviewBytes()!=std::vector<uint8_t>(16384,0x80))return 82;++reads;}
        Write(argv[5],std::to_string(reads));return reads?0:83;
    }
    Check(argc==2,"owned test directory argument required");const fs::path root=fs::absolute(argv[1]);
    Check(root.generic_string().find("/build/pack-codex-transactions-")!=std::string::npos,"tests must remain in own build tree");
    Check(!fs::exists(root),"fresh fixture root required");fs::create_directories(root);
    const auto oldSource=root/"source old",nextSource=root/"source new";Fixture(oldSource);Fixture(nextSource,newBytes);
    Group("positive install update catalogue lease uninstall",[&]{auto lib=root/"positive";std::string e;
        Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),"install: "+e);Exact(lib,oldBytes);
        auto lease=PackManager::LoadPack(Target(lib).u8string(),e);Check(bool(lease),e);
        Check(PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"update: "+e);Exact(lib,newBytes);
        auto cat=PackManager::GetCatalogue(lib.u8string());Check(cat.size()==1&&cat[0].isUsable&&cat[0].previewAssetId=="preview","catalogue identity");
        Check(PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"uninstall: "+e);
        auto old=lease->ReadAssetBytes("look");Check(std::string(old.begin(),old.end())==oldBytes,"retained lease changed");
        Check(lease->GetPreviewBytes()==std::vector<uint8_t>(16384,0x80),"retained preview changed");});
    Group("original spelling authority and owned prefix sentinel",[&]{std::string e;auto lib=root/"authority";
        for(auto bad:{std::string("relative"),std::string("D:relative"),(root/"x"/".."/"bad").u8string(),std::string("\\\\server\\share\\x"),(root/"bad.").u8string()}){
            Check(!PackManager::InstallPack(oldSource.u8string(),bad,e),"unsafe root accepted");}
        Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);auto sentinel=lib/".staging"/"staging_tx_foreign"/"keep.txt";Write(sentinel,"SENTINEL");
        PackManager::CleanOrphanStaging(lib.u8string());Check(Read(sentinel)=="SENTINEL","foreign prefix deleted");});
    Group("missing malformed mismatched uninstall never deletes",[&]{std::string e;auto lib=root/"uninstall";
        Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);auto manifest=Target(lib)/"manifest.json";auto original=Read(manifest);
        fs::remove(manifest);Check(!PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"missing manifest authorized deletion");Check(Read(Target(lib)/"look.cube")==oldBytes,"payload deleted");
        Write(manifest,"{}");Check(!PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"bad manifest authorized deletion");Write(manifest,original);
        Write(Target(lib)/"foreign.txt","KEEP");Check(!PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"undeclared sentinel authorized deletion");Check(Read(Target(lib)/"foreign.txt")=="KEEP","sentinel deleted");});
    Group("strict journal token schema grammar filename content binding",[&]{std::string e;auto lib=root/"grammar";Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
        PackManager::SetFaultSeam(PackFaultSeam::SimulateCrashAfterPrepared);Check(!PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"prepared seam false success");PackManager::SetFaultSeam(PackFaultSeam::None);
        fs::path journal;for(const auto& x:fs::directory_iterator(lib/".staging"))if(x.path().extension()==".json")journal=x.path();Check(!journal.empty(),"journal absent");auto good=Read(journal);
        std::vector<std::string> bad={good+"{}",good.substr(0,good.size()-3),"{\"schemaVersion\":2,\"schemaVersion\":2}",std::string(65537,' ')};
        for(const std::string key:{"schemaVersion","txId","state","packId","version","oldExists","newManifestSha256","oldManifestSha256","newContentSha256","oldContentSha256","timestamp","ownerToken"}){
            auto x=good;auto at=x.find("\""+key+"\"");Check(at!=std::string::npos,"field fixture");auto end=x.find('\n',at);x.erase(at,end-at+1);bad.push_back(x);}
        for(const auto& text:bad){Write(journal,text);Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"bad journal accepted");Check(fs::exists(journal),"invalid journal deleted");Exact(lib,oldBytes);}
        Write(journal,good);auto forged=journal.parent_path()/"journal_tx_00000000000000000000000000000000.json";fs::rename(journal,forged);
        Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"filename forgery accepted");fs::rename(forged,journal);
        Check(PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"valid prepared recovery: "+e);Exact(lib,oldBytes);});
    Group("all durable late transition and rollback faults preserve recoverable version",[&]{
        for(auto seam:{PackFaultSeam::SimulateJournalAfterBackupFailure,PackFaultSeam::SimulateJournalAfterPublishFailure,PackFaultSeam::SimulateJournalAfterCommitFailure,
            PackFaultSeam::SimulateCleanupFailure,PackFaultSeam::SimulateCommitRenameFailure,PackFaultSeam::SimulateRollbackFailure,
            PackFaultSeam::SimulateJournalWriteFailure,PackFaultSeam::SimulateJournalFlushFailure,PackFaultSeam::SimulateJournalRenameFailure}){
            auto lib=root/("fault_"+std::to_string(int(seam)));std::string e;Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
            PackManager::SetFaultSeam(seam);Check(!PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"fault false success");Check(!e.empty(),"fault lacks diagnostic");
            if(seam==PackFaultSeam::SimulateRollbackFailure)Check(e.find("restore failed")!=std::string::npos,"lost restore failure");
            PackManager::SetFaultSeam(PackFaultSeam::None);Check(PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"fault recovery: "+e);
            Exact(lib,(seam==PackFaultSeam::SimulateJournalAfterPublishFailure||seam==PackFaultSeam::SimulateJournalAfterCommitFailure||seam==PackFaultSeam::SimulateCleanupFailure)?newBytes:oldBytes);
        }});
    Group("actual killed publisher every physical journal gap fresh process recovery",[&]{
        for(bool update:{false,true})for(int phase=1;phase<=7;++phase){if(!update&&(phase==2||phase==3))continue;
            auto lib=root/("crash_"+std::to_string(update)+"_"+std::to_string(phase));std::string e;
            if(update)Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
            Events ev;auto child=Spawn(PublishArgs(nextSource,lib,phase,ev));ev.Wait();Check(child.Kill()==77,"actual publisher exit");
            auto log=root/("recovery_"+std::to_string(update)+"_"+std::to_string(phase)+".txt");auto recover=Spawn(L"--recover "+Quote(lib.u8string())+L" "+Quote(log.u8string()));
            Check(recover.Exit()==0,"fresh recovery failed: "+Read(log));
            if(phase>=4)Exact(lib,newBytes);else if(update)Exact(lib,oldBytes);else Check(!fs::exists(Target(lib)),"aborted first install published unexpectedly");
            std::cout<<"  killed PID="<<child.pid<<" checkpoint="<<phase<<" update="<<update<<" recoveryPID="<<recover.pid<<"\n";
        }});
    Group("namespace held operation contention timeout and real abandoned mutex",[&]{
        auto lib=root/"held";std::string e;Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);Write(lib/"sentinel.txt","KEEP");
        HANDLE keeper=CreateMutexW(nullptr,FALSE,W(MutexName(lib)).c_str());Check(keeper!=nullptr,"keeper mutex");Events ev;
        auto publisher=Spawn(PublishArgs(nextSource,lib,int(PackTransactionPhase::PreparedJournal),ev));ev.Wait();
        Check(!MoveFileExW(lib.c_str(),(root/"replaced").c_str(),0),"held library namespace replaced");Check(GetLastError()==ERROR_SHARING_VIOLATION||GetLastError()==ERROR_ACCESS_DENIED,"expected sharing denial");
        auto timeoutLog=root/"timeout.txt";auto contender=Spawn(L"--contend "+Quote(lib.u8string())+L" 150 "+Quote(timeoutLog.u8string()));
        Check(contender.Exit()==1,"contender did not timeout");Check(Read(timeoutLog).rfind(std::to_string(WAIT_TIMEOUT),0)==0,"actual WAIT_TIMEOUT absent");
        Check(publisher.Kill()==77,"abandoned publisher kill");auto abandonedLog=root/"abandoned.txt";
        auto recovery=Spawn(L"--recover "+Quote(lib.u8string())+L" "+Quote(abandonedLog.u8string()));Check(recovery.Exit()==0,"abandoned recovery failed");
        Check(Read(abandonedLog).rfind(std::to_string(WAIT_ABANDONED),0)==0,"actual WAIT_ABANDONED absent");CloseHandle(keeper);
        Check(Read(lib/"sentinel.txt")=="KEEP","sibling sentinel changed");Exact(lib,oldBytes);
    });
    Group("retained reader child overlaps real update and uninstall",[&]{auto lib=root/"reader";std::string e;Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
        Events ev;auto log=root/"reader-count.txt";auto reader=Spawn(L"--reader "+Quote(lib.u8string())+L" "+Quote(ev.a)+L" "+Quote(ev.b)+L" "+Quote(log.u8string()));ev.Wait();
        Check(PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"overlapped update: "+e);
        Check(PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"overlapped uninstall: "+e);
        ev.Release();Check(reader.Exit()==0,"immutable reader child failed");Check(std::stoull(Read(log))>0,"no reader overlap");std::cout<<"  immutable reader PID="<<reader.pid<<" reads="<<Read(log)<<"\n";
    });
    Group("every held ancestor staging target backup rejects namespace replacement",[&]{
        for(int phase:{int(PackTransactionPhase::PreparedJournal),int(PackTransactionPhase::BackupMoved)}){
            auto lib=root/("namespace_"+std::to_string(phase));std::string e;Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);Events ev;
            auto child=Spawn(PublishArgs(nextSource,lib,phase,ev));ev.Wait();
            std::vector<fs::path> protectedPaths={lib,lib.parent_path(),lib/".staging",lib/"com.prismforge.transactions"};
            if(fs::exists(Target(lib)))protectedPaths.push_back(Target(lib));
            for(const auto& entry:fs::directory_iterator(lib/".staging"))if(entry.is_directory())protectedPaths.push_back(entry.path());
            for(const auto& path:protectedPaths){Check(!MoveFileExW(PackStorage::NativePath(path).c_str(),PackStorage::NativePath(path.parent_path()/(path.filename().u8string()+"_replacement")).c_str(),0),"protected namespace replaced");}
            Check(child.Kill()==77,"namespace child kill");Check(PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"namespace recovery: "+e);Exact(lib,oldBytes);
        }});
    Group("reparse aliases preserve external sibling sentinels",[&]{
        auto external=root/"outside sentinel";Write(external/"keep.txt","EXTERNAL");std::string e;
        auto alias=root/"root alias";Check(Junction(alias,external),"junction fixture capability unavailable");
        Check(!PackManager::InstallPack(oldSource.u8string(),alias.u8string(),e),"root reparse accepted");
        Check(!PackManager::RecoverInterruptedTransactions((alias/"child").u8string(),e),"ancestor reparse accepted");
        auto lib=root/"reparse library";Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
        auto staging=lib/".staging";fs::rename(staging,lib/"preserved staging");Check(Junction(staging,external),"staging junction fixture");
        Check(!PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"staging reparse accepted");
        Check(Read(external/"keep.txt")=="EXTERNAL","external sentinel changed");Exact(lib,oldBytes);
    });
    Group("published committed missing corrupt wrong identity matrices preserve intact data",[&]{
        for(bool committed:{false,true})for(int variant=0;variant<4;++variant){
            auto lib=root/("matrix_"+std::to_string(committed)+"_"+std::to_string(variant));std::string e;Check(PackManager::InstallPack(oldSource.u8string(),lib.u8string(),e),e);
            PackManager::SetFaultSeam(committed?PackFaultSeam::SimulateCleanupFailure:PackFaultSeam::SimulateCrashAfterPublish);
            Check(!PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"matrix seam accepted");PackManager::SetFaultSeam(PackFaultSeam::None);
            fs::path backup;for(const auto& x:fs::directory_iterator(lib/".staging"))if(x.path().filename().u8string().rfind("backup_",0)==0)backup=x.path();Check(!backup.empty(),"matrix old backup absent");
            if(variant==0){fs::rename(Target(lib),lib/"preserved new target");Check(PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"missing new restore: "+e);Exact(lib,oldBytes);Check(Read(lib/"preserved new target"/"look.cube")==newBytes,"preserved displaced new target changed");}
            if(variant==1){Write(Target(lib)/"look.cube","CORRUPT");Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"corrupt new accepted");Check(Read(backup/"look.cube")==oldBytes,"sole intact old lost");Check(Read(Target(lib)/"look.cube")=="CORRUPT","unknown current target deleted");}
            if(variant==2){Write(backup/"look.cube","CORRUPT OLD");Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"corrupt old backup accepted");Exact(lib,newBytes);Check(Read(backup/"look.cube")=="CORRUPT OLD","corrupt backup discarded without identity");}
            if(variant==3){auto m=Fixture(backup,oldBytes);m.packId="com.prismforge.foreign";Check(ManifestJson::WriteToFile((backup/"manifest.json").u8string(),m,e),e);
                Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"wrong identity backup accepted");Exact(lib,newBytes);Check(fs::exists(backup/"manifest.json"),"wrong backup deleted");}
        }});
    Group("UTF8 Unicode library source and semantically equal casing share identity",[&]{
        auto lib=root/fs::u8path(u8"\u00e9tude_\u65e5\u672c_\u0394");auto source=root/fs::u8path(u8"\u6e90_\u00e9tude");Fixture(source);std::string e;
        Check(PackManager::InstallPack(source.u8string(),lib.u8string(),e),"Unicode install: "+e);
        auto loaded=PackManager::LoadPack(Target(lib).u8string(),e);Check(bool(loaded),"Unicode loaded: "+e);
        auto upper=lib.native();for(auto& c:upper)if(c>=L'a'&&c<=L'z')c=wchar_t(c-L'a'+L'A');
        Check(PackStorage::PathIdentity(lib)==PackStorage::PathIdentity(fs::path(upper)),"Windows case aliases disagree");
        Check(PackManager::RecoverInterruptedTransactions(fs::path(upper).u8string(),e),"Unicode alias recovery: "+e);
        Check(PackManager::UninstallPack(lib.u8string(),"com.prismforge.transactions","1.0.0",e),"Unicode uninstall: "+e);
    });
    Group("missing published first install preserves actionable journal",[&]{
        for(bool committed:{false,true}){auto lib=root/("missing first_"+std::to_string(committed));std::string e;
            PackManager::SetFaultSeam(committed?PackFaultSeam::SimulateCleanupFailure:PackFaultSeam::SimulateCrashAfterPublish);
            Check(!PackManager::InstallPack(nextSource.u8string(),lib.u8string(),e),"first publish fault false success");PackManager::SetFaultSeam(PackFaultSeam::None);
            fs::rename(Target(lib),lib/"preserved new");Check(!PackManager::RecoverInterruptedTransactions(lib.u8string(),e),"missing first publish reported recovered");
            Check(!e.empty(),"missing first publish has no diagnostic");bool journal=false;for(const auto& x:fs::directory_iterator(lib/".staging"))if(x.path().extension()==".json")journal=true;
            Check(journal,"missing first publish discarded journal");Check(Read(lib/"preserved new"/"look.cube")==newBytes,"preserved first-install bytes changed");
        }});
    Group("ZIP Windows reserved components fail before any entry write",[&]{
        wxInitializer initialized;Check(initialized.IsOk(),"ZIP fixture runtime");
        for(const std::string name:{"NUL","CON.cube","sub/PRN.json"}){
            auto archive=root/("reserved_"+std::to_string(checks)+".zip");
            {wxFileOutputStream file(wxString::FromUTF8(archive.u8string()));Check(file.IsOk(),"ZIP fixture file");
                wxZipOutputStream zip(file);Check(zip.PutNextEntry(wxString::FromUTF8(name)),"ZIP fixture entry");
                const char payload[]="NEVER WRITE RESERVED";zip.Write(payload,sizeof(payload)-1);Check(zip.Close(),"ZIP fixture close");}
            auto library=root/("reserved_library_"+std::to_string(checks));std::string e;
            Check(!PackManager::InstallPackFromArchive(archive.u8string(),library.u8string(),e),"reserved ZIP accepted");
            Check(e.find("Unsafe ZIP entry component before extraction")!=std::string::npos,"reserved ZIP was only rejected after entry write: "+e);
        }});
    std::cout<<"SUMMARY groups="<<groups<<" assertions="<<checks<<" failures="<<failures<<"\n";return failures?1:0;
}catch(const std::exception& e){std::cerr<<"fatal: "<<e.what()<<"\n";return 2;}}
