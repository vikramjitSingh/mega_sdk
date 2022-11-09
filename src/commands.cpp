/**
 * @file commands.cpp
 * @brief Implementation of various commands
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/types.h"
#include "mega/command.h"
#include "mega/megaapp.h"
#include "mega/fileattributefetch.h"
#include "mega/base64.h"
#include "mega/transferslot.h"
#include "mega/transfer.h"
#include "mega/utils.h"
#include "mega/user.h"
#include "mega.h"
#include "mega/mediafileattribute.h"
#include "mega/heartbeats.h"

namespace mega {
HttpReqCommandPutFA::HttpReqCommandPutFA(NodeOrUploadHandle cth, fatype ctype, bool usehttps, int ctag, size_t size_only, std::unique_ptr<string> cdata, bool getIP, HttpReqCommandPutFA::Cb &&completion)
    : mCompletion(std::move(completion)), data(std::move(cdata))
{
    assert(!!size_only ^ !!data);   // get URL or upload data, not both
    assert(!!mCompletion ^ !!data);  // completion and upload are incompatible
    cmd("ufa");
    arg("s", data ? data->size() : size_only);

    if (cth.isNodeHandle())
    {
        arg("h", cth.nodeHandle());
    }

    progressreported = 0;
    persistent = true;  // object will be recycled either for retry or for
                        // posting to the file attribute server

    if (usehttps)
    {
        arg("ssl", 2);
    }

    if (getIP)
    {
        arg("v", 3);
    }

    th = cth;
    type = ctype;

    binary = true;

    tag = ctag;

    if (!mCompletion)
    {
        mCompletion = [this](Error e, const std::string & /*url*/, const vector<std::string> & /*ips*/)
        {
            if (!data || data->empty())
            {
                e = API_EARGS;
                LOG_err << "Data object is " << (!data ? "nullptr" : "empty");
            }

            if (e == API_OK)
            {
                LOG_debug << "Sending file attribute data";
                progressreported = 0;
                HttpReq::type = REQ_BINARY;
                post(client, data->data(), static_cast<unsigned>(data->size()));
            }
            else
            {
                client->app->putfa_result(th.nodeHandle().as8byte(), type, e);
            }
        };
    }
}

bool HttpReqCommandPutFA::procresult(Result r)
{
    client->looprequested = true;

    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_EAGAIN) || r.wasError(API_ERATELIMIT))
        {
            status = REQ_FAILURE;
        }
        else
        {
            if (r.wasError(API_EACCESS))
            {
                // create a custom attribute indicating thumbnail can't be restored from this account
                Node *n = client->nodeByHandle(th.nodeHandle());

                char me64[12];
                Base64::btoa((const byte*)&client->me, MegaClient::USERHANDLE, me64);

                if (n && client->checkaccess(n, FULL) &&
                        (n->attrs.map.find('f') == n->attrs.map.end() || n->attrs.map['f'] != me64) )
                {
                    LOG_debug << "Restoration of file attributes is not allowed for current user (" << me64 << ").";

                    // 'canChangeVault' is false here because restoration of file attributes is triggered by
                    // downloads, so it cannot be triggered by a Backup operation
                    bool canChangeVault = false;
                    client->setattr(n, attr_map('f', me64), 0, nullptr, nullptr, canChangeVault);
                }
            }

            status = REQ_SUCCESS;
            mCompletion(r.errorOrOK(), {}, {});
        }
        return true;
    }
    else
    {
        const char* p = NULL;
        std::vector<string> ips;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case 'p':
                    p = client->json.getvalue();
                    break;

                case MAKENAMEID2('i', 'p'):
                    loadIpsFromJson(ips);
                    break;

                case EOO:
                    if (!p)
                    {
                        status = REQ_FAILURE;
                    }
                    else
                    {
                        JSON::copystring(&posturl, p);

                        // cache resolved URLs if received
                        std::vector<string> urls(1, posturl);
                        std::vector<string> ipsCopy = ips;

                        if(!cacheresolvedurls(urls, std::move(ips)))
                        {
                            LOG_err << "Unpaired IPs received for URLs in `ufa` command. URLs: " << urls.size() << " IPs: " << ips.size();
                        }

                        mCompletion(API_OK, posturl, ipsCopy);

                        return true;
                    }
                    break;

                default:
                    if (!client->json.storeobject())
                    {
                        status = REQ_SUCCESS;
                        mCompletion(API_EINTERNAL, {}, {});
                        return false;
                    }
            }
        }
    }
}

m_off_t HttpReqCommandPutFA::transferred(MegaClient *client)
{
    if (httpiohandle)
    {
        client->httpio->postpos(httpiohandle);
        return true;
    }

    return 0;
}

CommandGetFA::CommandGetFA(MegaClient *client, int p, handle fahref)
{
    part = p;

    cmd("ufa");
    arg("fah", (byte*)&fahref, sizeof fahref);

    if (client->usehttps)
    {
        arg("ssl", 2);
    }

    arg("r", 1);
}

bool CommandGetFA::procresult(Result r)
{
    fafc_map::iterator it = client->fafcs.find(part);
    client->looprequested = true;

    if (r.wasErrorOrOK())
    {
        if (it != client->fafcs.end())
        {
            faf_map::iterator fafsit;
            for (fafsit = it->second->fafs[0].begin(); fafsit != it->second->fafs[0].end(); )
            {
                // move from fresh to pending
                it->second->fafs[1][fafsit->first] = fafsit->second;
                it->second->fafs[0].erase(fafsit++);
            }

            it->second->e = r.errorOrOK();
            it->second->req.status = REQ_FAILURE;
        }

        return true;
    }

    const char* p = NULL;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'p':
                p = client->json.getvalue();
                break;

            case EOO:
                if (it != client->fafcs.end())
                {
                    if (p)
                    {
                        JSON::copystring(&it->second->posturl, p);
                        it->second->urltime = Waiter::ds;
                        it->second->dispatch();
                    }
                    else
                    {
                        faf_map::iterator fafsit;
                        for (fafsit = it->second->fafs[0].begin(); fafsit != it->second->fafs[0].end(); )
                        {
                            // move from fresh to pending
                            it->second->fafs[1][fafsit->first] = fafsit->second;
                            it->second->fafs[0].erase(fafsit++);
                        }

                        it->second->e = API_EINTERNAL;
                        it->second->req.status = REQ_FAILURE;
                    }
                }

                return true;

            default:
                if (!client->json.storeobject())
                {
                    faf_map::iterator fafsit;
                    for (fafsit = it->second->fafs[0].begin(); fafsit != it->second->fafs[0].end(); )
                    {
                        // move from fresh to pending
                        it->second->fafs[1][fafsit->first] = fafsit->second;
                        it->second->fafs[0].erase(fafsit++);
                    }

                    it->second->e = API_EINTERNAL;
                    it->second->req.status = REQ_FAILURE;
                    return false;
                }
        }
    }
}

CommandAttachFA::CommandAttachFA(MegaClient *client, handle nh, fatype t, handle ah, int ctag)
{
    cmd("pfa");
    notself(client);

    arg("n", (byte*)&nh, MegaClient::NODEHANDLE);

    char buf[64];

    sprintf(buf, "%u*", t);
    Base64::btoa((byte*)&ah, sizeof(ah), strchr(buf + 2, 0));
    arg("fa", buf);

    h = nh;
    type = t;
    tag = ctag;
}

CommandAttachFA::CommandAttachFA(MegaClient *client, handle nh, fatype t, const std::string& encryptedAttributes, int ctag)
{
    cmd("pfa");
    notself(client);

    arg("n", (byte*)&nh, MegaClient::NODEHANDLE);

    arg("fa", encryptedAttributes.c_str());

    h = nh;
    type = t;
    tag = ctag;
}

bool CommandAttachFA::procresult(Result r)
{
    if (!r.wasErrorOrOK())
    {
         string fa;
         if (client->json.storeobject(&fa))
         {
             Node* n = client->nodebyhandle(h);
             if (n)
             {
                n->fileattrstring = fa;
                n->changed.fileattrstring = true;
                client->notifynode(n);
             }
             client->app->putfa_result(h, type, API_OK);
             return true;
         }
    }

    client->app->putfa_result(h, type, r.errorOrOK());
    return r.wasErrorOrOK();
}

// request upload target URL
CommandPutFile::CommandPutFile(MegaClient* client, TransferSlot* ctslot, int ms)
{
    tslot = ctslot;

    cmd("u");

    if (client->usehttps)
    {
        arg("ssl", 2);
    }

    arg("v", 3);
    arg("s", tslot->fa->size);
    arg("ms", ms);

    // send minimum set of different tree's roots for API to check overquota
    set<handle> targetRoots;
    bool begun = false;
    for (auto &file : tslot->transfer->files)
    {
        if (!file->h.isUndef())
        {
            Node *node = client->nodeByHandle(file->h);
            if (node)
            {
                handle rootnode = client->getrootnode(node)->nodehandle;
                if (targetRoots.find(rootnode) != targetRoots.end())
                {
                    continue;
                }

                targetRoots.insert(rootnode);
            }
            if (!begun)
            {
                beginarray("t");
                begun = true;
            }

            element((byte*)&file->h, MegaClient::NODEHANDLE);
        }
    }

    if (begun)
    {
        endarray();
    }
    else
    {
        // Target user goes alone, not inside an array. Note: we are skipping this if a)more than two b)the array had been created for node handles
        for (auto &file : tslot->transfer->files)
        {
            if (file->h.isUndef() && file->targetuser.size())
            {
                arg("t", file->targetuser.c_str());
                break;
            }
        }
    }
}

void CommandPutFile::cancel()
{
    Command::cancel();
    tslot = NULL;
}

// set up file transfer with returned target URL
bool CommandPutFile::procresult(Result r)
{
    if (tslot)
    {
        tslot->pendingcmd = NULL;
    }
    else
    {
        canceled = true;
    }

    if (r.wasErrorOrOK())
    {
        if (!canceled)
        {
            tslot->transfer->failed(r.errorOrOK(), *client->mTctableRequestCommitter);
        }

        return true;
    }

    std::vector<std::string> tempurls;
    std::vector<std::string> tempips;
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'p':
                tempurls.push_back("");
                client->json.storeobject(canceled ? NULL : &tempurls.back());
                break;

            case MAKENAMEID2('i', 'p'):
                loadIpsFromJson(tempips);
                break;
            case EOO:
                if (canceled) return true;

                if (tempurls.size() == 1)
                {
                    if(!cacheresolvedurls(tempurls, std::move(tempips)))
                    {
                        LOG_err << "Unpaired IPs received for URLs in `u` command. URLs: " << tempurls.size() << " IPs: " << tempips.size();
                    }

                    tslot->transfer->tempurls = tempurls;
                    tslot->transferbuf.setIsRaid(tslot->transfer, tempurls, tslot->transfer->pos, tslot->maxRequestSize);
                    tslot->starttime = tslot->lastdata = client->waiter->ds;
                    tslot->progress();
                }
                else
                {
                    tslot->transfer->failed(API_EINTERNAL, *client->mTctableRequestCommitter);
                }
                return true;

            default:
                if (!client->json.storeobject())
                {
                    if (!canceled)
                    {
                        tslot->transfer->failed(API_EINTERNAL, *client->mTctableRequestCommitter);
                    }

                    return false;
                }
        }
    }
}

// request upload target URL
CommandGetPutUrl::CommandGetPutUrl(m_off_t size, int putmbpscap, bool forceSSL, bool getIP, CommandGetPutUrl::Cb completion)
    : mCompletion(completion)
{
    cmd("u");
    if (forceSSL)
    {
        arg("ssl", 2);
    }
    if (getIP)
    {
        arg("v", 3);
    }
    else
    {
        arg("v", 2);
    }
    arg("s", size);
    arg("ms", putmbpscap);
}


// set up file transfer with returned target URL
bool CommandGetPutUrl::procresult(Result r)
{
    string url;
    std::vector<string> ips;

    if (r.wasErrorOrOK())
    {
        if (!canceled)
        {
            mCompletion(r.errorOrOK(), url, ips);
        }
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'p':
                client->json.storeobject(canceled ? nullptr : &url);
                break;
            case MAKENAMEID2('i', 'p'):
                loadIpsFromJson(ips);
                break;
            case EOO:
                if (canceled) return true;
                mCompletion(API_OK, url, ips);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    if (!canceled)
                    {
                        mCompletion(API_EINTERNAL, string(), {});
                    }
                    return false;
                }
        }
    }
}

// request temporary source URL for DirectRead
CommandDirectRead::CommandDirectRead(MegaClient *client, DirectReadNode* cdrn)
{
    drn = cdrn;

    cmd("g");
    arg(drn->p ? "n" : "p", (byte*)&drn->h, MegaClient::NODEHANDLE);
    arg("g", 1); // server will provide download URL(s)/token(s) (if skipped, only information about the file)
    arg("v", 2);  // version 2: server can supply details for cloudraid files

    if (drn->privateauth.size())
    {
        arg("esid", drn->privateauth.c_str());
    }

    if (drn->publicauth.size())
    {
        arg("en", drn->publicauth.c_str());
    }

    if (drn->chatauth.size())
    {
        arg("cauth", drn->chatauth.c_str());
    }

    if (client->usehttps)
    {
        arg("ssl", 2);
    }
}

void CommandDirectRead::cancel()
{
    Command::cancel();
    drn = NULL;
}

bool CommandDirectRead::procresult(Result r)
{
    if (drn)
    {
        drn->pendingcmd = NULL;
    }

    if (r.wasErrorOrOK())
    {
        if (!canceled && drn)
        {
            drn->cmdresult(r.errorOrOK());
        }
        return true;
    }
    else
    {
        Error e(API_EINTERNAL);
        dstime tl = 0;
        std::vector<std::string> tempurls;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case 'g':
                    if (client->json.enterarray())   // now that we are requesting v2, the reply will be an array of 6 URLs for a raid download, or a single URL for the original direct download
                    {
                        for (;;)
                        {
                            std::string tu;
                            if (!client->json.storeobject(&tu))
                            {
                                break;
                            }
                            tempurls.push_back(tu);
                        }
                        client->json.leavearray();
                    }
                    else
                    {
                        std::string tu;
                        if (client->json.storeobject(&tu))
                        {
                            tempurls.push_back(tu);
                        }
                    }
                    if (tempurls.size() == 1 || tempurls.size() == RAIDPARTS)
                    {
                        if (drn)
                        {
                            drn->tempurls.swap(tempurls);
                            e.setErrorCode(API_OK);
                        }
                    }
                    else
                    {
                        e.setErrorCode(API_EINCOMPLETE);
                    }
                    break;

                case 's':
                    if (drn)
                    {
                        drn->size = client->json.getint();
                    }
                    break;

                case 'd':
                    e = API_EBLOCKED;
                    break;

                case 'e':
                    e = (error)client->json.getint();
                    break;

                case MAKENAMEID2('t', 'l'):
                    tl = dstime(client->json.getint());
                    break;

                case EOO:
                    if (!canceled && drn)
                    {
                        if (e == API_EOVERQUOTA && !tl)
                        {
                            // default retry interval
                            tl = MegaClient::DEFAULT_BW_OVERQUOTA_BACKOFF_SECS;
                        }

                        drn->cmdresult(e, e == API_EOVERQUOTA ? tl * 10 : 0);
                    }

                    return true;

                default:
                    if (!client->json.storeobject())
                    {
                        if (!canceled && drn)
                        {
                            drn->cmdresult(e);
                        }

                        return false;
                    }
            }
        }
    }
}

// request temporary source URL for full-file access (p == private node)
CommandGetFile::CommandGetFile(MegaClient *client, const byte* key, size_t keySize,
                               handle h, bool p, const char *privateauth,
                               const char *publicauth, const char *chatauth,
                               bool singleUrl, Cb &&completion)
{
    cmd("g");
    arg(p ? "n" : "p", (byte*)&h, MegaClient::NODEHANDLE);
    arg("g", 1); // server will provide download URL(s)/token(s) (if skipped, only information about the file)
    if (!singleUrl)
    {
        arg("v", 2);  // version 2: server can supply details for cloudraid files
    }

    if (client->usehttps)
    {
        arg("ssl", 2);
    }

    if (privateauth)
    {
        arg("esid", privateauth);
    }

    if (publicauth)
    {
        arg("en", publicauth);
    }

    if (chatauth)
    {
        arg("cauth", chatauth);
    }

    assert(key && "no key provided!");
    if (key && keySize != SymmCipher::KEYLENGTH)
    {
        assert (keySize <= FILENODEKEYLENGTH);
        memcpy(filekey, key, keySize);
        mFileKeyType = FILENODE;
    }
    else if (key && keySize == SymmCipher::KEYLENGTH)
    {
        memcpy(filekey, key, SymmCipher::KEYLENGTH);
        mFileKeyType = 1;
    }

    mCompletion = std::move(completion);
}

void CommandGetFile::cancel()
{
    Command::cancel();
}


void CommandGetFile::callFailedCompletion(const Error &e)
{
    assert(mCompletion);
    if (mCompletion)
    {
        mCompletion(e, -1, -1, -1, 0, nullptr, nullptr, nullptr, {}, {});
    }
}

// process file credentials
bool CommandGetFile::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (!canceled)
        {
            callFailedCompletion(r.errorOrOK());
        }
        return true;
    }

    const char* at = nullptr;
    Error e(API_EINTERNAL);
    m_off_t s = -1;
    dstime tl = 0;
    std::unique_ptr<byte[]> buf;
    m_time_t ts = 0, tm = 0;

    // credentials relevant to a non-TransferSlot scenario (node query)
    string fileattrstring;
    string filenamestring;
    string filefingerprint;
    vector<string> tempurls;
    vector<string> tempips;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'g':
                if (client->json.enterarray())   // now that we are requesting v2, the reply will be an array of 6 URLs for a raid download, or a single URL for the original direct download
                {
                    for (;;)
                    {
                        std::string tu;
                        if (!client->json.storeobject(&tu))
                        {
                            break;
                        }
                        tempurls.push_back(tu);
                    }
                    client->json.leavearray();
                }
                else
                {
                    std::string tu;
                    if (client->json.storeobject(&tu))
                    {
                        tempurls.push_back(tu);
                    }
                }
                e.setErrorCode(API_OK);
                break;

            case MAKENAMEID2('i', 'p'):
                loadIpsFromJson(tempips);
                break;

            case 's':
                s = client->json.getint();
                break;

            case MAKENAMEID2('t', 's'):
                ts = client->json.getint();
                break;

            case MAKENAMEID3('t', 'm', 'd'):
                tm = ts + client->json.getint();
                break;

            case MAKENAMEID2('a', 't'):
                at = client->json.getvalue();
                break;

            case MAKENAMEID2('f', 'a'):
                client->json.storeobject(&fileattrstring);
                break;

            case 'e':
                e = (error)client->json.getint();
                break;

            case MAKENAMEID2('t', 'l'):
                tl = dstime(client->json.getint());
                break;

            case EOO:
            {
                // defer code that steals the ips <move(tempips)> and stores them in the cache
                // thus we can use them before going out of scope
                std::shared_ptr<void> deferThis(nullptr, [this, &tempurls, &tempips](...)
                {
                    if(!cacheresolvedurls(tempurls, std::move(tempips)))
                    {
                        LOG_err << "Unpaired IPs received for URLs in `g` command. URLs: " << tempurls.size() << " IPs: " << tempips.size();
                    }
                });

                if (canceled) //do not proceed: SymmCipher may no longer exist
                {
                    return true;
                }

                if (!at)
                {
                    callFailedCompletion(API_EINTERNAL);
                    return true;
                }

                // decrypt at and set filename
                SymmCipher * cipherer = client->getRecycledTemporaryTransferCipher(filekey, mFileKeyType);
                const char* eos = strchr(at, '"');
                buf.reset(Node::decryptattr(cipherer, at, eos ? eos - at : strlen(at)));
                if (!buf)
                {
                    callFailedCompletion(API_EKEY);
                    return true;
                }

                // all good, lets parse the attribute string
                JSON json;
                json.begin((char*)buf.get() + 5);

                for (;;)
                {
                    switch (json.getnameid())
                    {
                        case 'c':
                            if (!json.storeobject(&filefingerprint))
                            {
                                callFailedCompletion(API_EINTERNAL);
                                return true;
                            }
                            break;

                        case 'n':
                            if (!json.storeobject(&filenamestring))
                            {
                                callFailedCompletion(API_EINTERNAL);
                                return true;
                            }
                            break;

                        case EOO:
                            { //succeded, call completion function!
                                return mCompletion ? mCompletion(e, s, ts, tm, tl,
                                            &filenamestring, &filefingerprint, &fileattrstring,
                                            tempurls, tempips) : false;
                            }

                        default:
                            if (!json.storeobject())
                            {
                                callFailedCompletion(API_EINTERNAL);
                                return false;
                            }
                    }
                }
            }
            default:
                if (!client->json.storeobject())
                {
                    if (!canceled)
                    {
                        callFailedCompletion(API_EINTERNAL);
                    }
                    return false;
                }
        }
    }
}

CommandSetAttr::CommandSetAttr(MegaClient* client, Node* n, SymmCipher* cipher, const char* prevattr, Completion&& c, bool canChangeVault)
{
    cmd("a");
    notself(client);

    string at;

    n->attrs.getjson(&at);
    client->makeattr(cipher, &at, at.c_str(), int(at.size()));

    arg("n", (byte*)&n->nodehandle, MegaClient::NODEHANDLE);
    arg("at", (byte*)at.c_str(), int(at.size()));

    if (canChangeVault)
    {
        arg("vw", 1);
    }

    h = n->nodeHandle();
    tag = 0;
    syncop = prevattr;

    if(prevattr)
    {
        pa = prevattr;
    }

    completion = move(c);
}

bool CommandSetAttr::procresult(Result r)
{
#ifdef ENABLE_SYNC
    if(r.wasError(API_OK) && syncop)
    {
        Node* node = client->nodeByHandle(h);
        if(node)
        {
            // After speculative instant completion removal, this is not needed (always sent via actionpacket code)
            LOG_debug << "Sync - remote rename from " << pa << " to " << node->displayname();
        }
    }
#endif
    if (completion) completion(h, r.errorOrOK());
    return r.wasErrorOrOK();
}

// (the result is not processed directly - we rely on the server-client
// response)
CommandPutNodes::CommandPutNodes(MegaClient* client, NodeHandle th,
                                 const char* userhandle, VersioningOption vo,
                                 vector<NewNode>&& newnodes, int ctag, putsource_t csource, const char *cauth,
                                 Completion&& resultFunction, bool canChangeVault)
  : mResultFunction(resultFunction)
{
    byte key[FILENODEKEYLENGTH];

#ifdef DEBUG
    assert(newnodes.size() > 0);
    for (auto& n : newnodes) assert(n.canChangeVault == canChangeVault);
#endif

    nn = std::move(newnodes);
    type = userhandle ? USER_HANDLE : NODE_HANDLE;
    source = csource;
    cmd("p");
    notself(client);

    if (userhandle)
    {
        arg("t", userhandle);
        targethandle.setUndef();
    }
    else
    {
        arg("t", (byte*)&th, MegaClient::NODEHANDLE);
        targethandle = th;
    }

    arg("sm",1);

    if (cauth)
    {
        arg("cauth", cauth);
    }

    if (canChangeVault)
    {
        arg("vw", 1);
    }

    // "vb": when provided, it force to override the account-wide versioning behavior by the value indicated by client
    //     vb:1 to force it on
    //     vb:0 to force it off
    // Dont provide it at all to rely on the account-wide setting (as of the moment the command is processed).
    switch (vo)
    {
        case NoVersioning:
            break;

        case ClaimOldVersion:
            arg("vb", 1);
            break;

        case ReplaceOldVersion:
            arg("vb", m_off_t(0));
            break;

        case UseLocalVersioningFlag:
            arg("vb", !client->versions_disabled);
            vo = !client->versions_disabled ? ClaimOldVersion : ReplaceOldVersion;
            break;

        case UseServerVersioningFlag:
            break;
    }

    beginarray("n");

    for (unsigned i = 0; i < nn.size(); i++)
    {
        beginobject();

        NewNode* nni = &nn[i];
        switch (nni->source)
        {
            case NEW_NODE:
                arg("h", (byte*)&nni->nodehandle, MegaClient::NODEHANDLE);
                break;

            case NEW_PUBLIC:
                arg("ph", (byte*)&nni->nodehandle, MegaClient::NODEHANDLE);
                break;

            case NEW_UPLOAD:
                arg("h", nni->uploadtoken.data(), sizeof nn[0].uploadtoken);

                // include pending file attributes for this upload
                string s;

                if (nni->fileattributes)
                {
                    // if attributes are set on the newnode then the app is not using the pendingattr mechanism
                    s.swap(*nni->fileattributes);
                    nni->fileattributes.reset();
                }
                else
                {
                    client->pendingattrstring(nn[i].uploadhandle, &s);

#ifdef USE_MEDIAINFO
                    client->mediaFileInfo.addUploadMediaFileAttributes(nn[i].uploadhandle, &s);
#endif
                }

                if (s.size())
                {
                    arg("fa", s.c_str(), 1);
                }
        }

        if (!ISUNDEF(nn[i].parenthandle))
        {
            arg("p", (byte*)&nn[i].parenthandle, MegaClient::NODEHANDLE);
        }

        if (vo != NoVersioning &&
            nn[i].type == FILENODE && !nn[i].ovhandle.isUndef())
        {
            arg("ov", (byte*)&nn[i].ovhandle, MegaClient::NODEHANDLE);
        }
        nn[i].mVersioningOption = vo;

        arg("t", nn[i].type);
        arg("a", (byte*)nn[i].attrstring->data(), int(nn[i].attrstring->size()));

        if (!client->loggedIntoWritableFolder())
        {
            if (nn[i].nodekey.size() <= sizeof key)
            {
                client->key.ecb_encrypt((byte*)nn[i].nodekey.data(), key, nn[i].nodekey.size());
                arg("k", key, int(nn[i].nodekey.size()));
            }
            else
            {
                arg("k", (const byte*)nn[i].nodekey.data(), int(nn[i].nodekey.size()));
            }
        }
        endobject();
    }

    endarray();

    // add cr element for new nodes, if applicable
    if (type == NODE_HANDLE)
    {
        Node* tn;
        if ((tn = client->nodeByHandle(th)))
        {
            ShareNodeKeys snk;

            for (unsigned i = 0; i < nn.size(); i++)
            {
                switch (nn[i].source)
                {
                    case NEW_PUBLIC:
                    case NEW_NODE:
                        snk.add(nn[i].nodekey, nn[i].nodehandle, tn, 0);
                        break;

                    case NEW_UPLOAD:
                        snk.add(nn[i].nodekey, nn[i].nodehandle, tn, 0, nn[i].uploadtoken.data(), (int)sizeof nn[i].uploadtoken);
                        break;
                }
            }

            snk.get(this, true);
        }
    }

    tag = ctag;
}

// add new nodes and handle->node handle mapping
void CommandPutNodes::removePendingDBRecordsAndTempFiles()
{
    pendingdbid_map::iterator it = client->pendingtcids.find(tag);
    if (it != client->pendingtcids.end())
    {
        if (client->tctable)
        {
            client->mTctableRequestCommitter->beginOnce();
            vector<uint32_t> &ids = it->second;
            for (unsigned int i = 0; i < ids.size(); i++)
            {
                if (ids[i])
                {
                    client->tctable->del(ids[i]);
                }
            }
        }
        client->pendingtcids.erase(it);
    }
    pendingfiles_map::iterator pit = client->pendingfiles.find(tag);
    if (pit != client->pendingfiles.end())
    {
        vector<LocalPath> &pfs = pit->second;
        for (unsigned int i = 0; i < pfs.size(); i++)
        {
            client->fsaccess->unlinklocal(pfs[i]);
        }
        client->pendingfiles.erase(pit);
    }
}

bool CommandPutNodes::procresult(Result r)
{
    removePendingDBRecordsAndTempFiles();

    if (r.wasErrorOrOK())
    {
        LOG_debug << "Putnodes error " << r.errorOrOK();
        if (r.wasError(API_EOVERQUOTA))
        {
            if (client->isPrivateNode(targethandle))
            {
                client->activateoverquota(0, false);
            }
#ifdef ENABLE_SYNC
            else    // the target's account is overquota
            {
                if (source == PUTNODES_SYNC)
                {
                    client->disableSyncContainingNode(targethandle, FOREIGN_TARGET_OVERSTORAGE, false);
                }
            }
#endif
        }
#ifdef ENABLE_SYNC
        if (source == PUTNODES_SYNC)
        {
            if (r.wasError(API_EACCESS))
            {
                client->sendevent(99402, "API_EACCESS putting node in sync transfer", 0);
            }

            vector<NewNode> emptyVec;

            if (mResultFunction) mResultFunction(r.errorOrOK(), type, emptyVec, false);
            else client->app->putnodes_result(r.errorOrOK(), type, emptyVec);

            for (size_t i = 0; i < nn.size(); i++)
            {
                nn[i].localnode.reset();
            }

            client->putnodes_sync_result(r.errorOrOK(), nn);
            return true;
        }
        else
        {
#endif
            if (source == PUTNODES_APP)
            {
                if (mResultFunction) mResultFunction(r.errorOrOK(), type, nn, false);
                else client->app->putnodes_result(r.errorOrOK(), type, nn);

                return true;
            }
#ifdef ENABLE_SYNC
            else
            {
                client->putnodes_syncdebris_result(r.errorOrOK(), nn);
                return true;
            }
        }
#endif
    }

    Error e = API_EINTERNAL;
    bool noexit = true;
    bool empty = false;
    while (noexit)
    {
        switch (client->json.getnameid())
        {
            case 'f':
                empty = !memcmp(client->json.pos, "[]", 2);
                if (client->readnodes(&client->json, 1, source, &nn, tag, true))  // do apply keys to received nodes only as we go for command response, much much faster for many small responses
                {
                    e = API_OK;
                }
                else
                {
                    LOG_err << "Parse error (readnodes)";
                    e = API_EINTERNAL;
                    noexit = false;
                }
                break;

            case MAKENAMEID2('f', '2'):
                if (!client->readnodes(&client->json, 1, PUTNODES_APP, nullptr, 0, true))  // do apply keys to received nodes only as we go for command response, much much faster for many small responses
                {
                    LOG_err << "Parse error (readversions)";
                    e = API_EINTERNAL;
                    noexit = false;
                }
                break;

            default:
                if (client->json.storeobject())
                {
                    continue;
                }

                e = API_EINTERNAL;
                LOG_err << "Parse error (PutNodes)";

                // fall through
            case EOO:
                noexit = false;
                break;
        }
    }

    client->sendkeyrewrites();

    // when the target has been removed, the API automatically adds the new node/s
    // into the rubbish bin
    Node *tempNode = !nn.empty() ? client->nodebyhandle(nn.front().mAddedHandle) : nullptr;
    bool targetOverride = (tempNode && tempNode->parenthandle != targethandle.as8byte());

#ifdef ENABLE_SYNC
    if (source == PUTNODES_SYNC)
    {
        if (mResultFunction) mResultFunction(e, type, nn, targetOverride);
        else client->app->putnodes_result(e, type, nn, targetOverride);

        client->putnodes_sync_result(e, nn);
    }
    else
#endif
    if (source == PUTNODES_APP)
    {
#ifdef ENABLE_SYNC
        if (!targethandle.isUndef())
        {
            Node *parent = client->nodeByHandle(targethandle);
            if (parent && parent->localnode)
            {
                // A node has been added by a regular (non sync) putnodes
                // inside a synced folder, so force a syncdown to detect
                // and sync the changes.
                client->syncdownrequired = true;
            }
        }
#endif
        auto ec = (!e && empty) ? API_ENOENT : static_cast<error>(e);

        if (mResultFunction) mResultFunction(ec, type, nn, targetOverride);
        else client->app->putnodes_result((!e && empty) ? API_ENOENT : static_cast<error>(e), type, nn, targetOverride);
    }
#ifdef ENABLE_SYNC
    else
    {
        client->putnodes_syncdebris_result(e, nn);
    }
#endif
    return true;
}

CommandMoveNode::CommandMoveNode(MegaClient* client, Node* n, Node* t, syncdel_t csyncdel, NodeHandle prevparent, Completion&& c, bool canChangeVault)
{
    h = n->nodeHandle();
    syncdel = csyncdel;
    np = t->nodeHandle();
    pp = prevparent;
    syncop = !pp.isUndef();
    mCanChangeVault = canChangeVault;

    cmd("m");

    // Special case for Move, we do set the 'i' field.
    // This is needed for backward compatibility, old versions used memcmp to detect if a 'd' actionpacket was followed by a 't'  actionpacket with the same 'i' (ie, a move)
    // Additionally the servers can't deliver `st` in that packet for the same reason.  And of course we will not ignore this `t` packet, despite setting 'i'.
    notself(client);

    if (mCanChangeVault)
    {
        arg("vw", 1);
    }

    arg("n", h);
    arg("t", t->nodeHandle());
    assert(t->type != FILENODE);

    TreeProcShareKeys tpsk;
    client->proctree(n, &tpsk);
    tpsk.get(this);

    tag = client->reqtag;
    completion = move(c);
}

bool CommandMoveNode::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_EOVERQUOTA))
        {
            client->activateoverquota(0, false);
        }

#ifdef ENABLE_SYNC
        if (syncdel != SYNCDEL_NONE)
        {
            Node* syncn = client->nodeByHandle(h);

            if (syncn)
            {
                if (r.wasError(API_OK))
                {
                    // update all todebris records in the subtree
                    for (auto it = client->toDebris.begin(); it != client->toDebris.end(); it++)
                    {
                        Node* toDebrisNode = it->first;
                        Node* n = it->first;

                        do {
                            if (n == syncn)
                            {
                                if (syncop)
                                {
                                    // After speculative instant completion removal, this is not needed (always sent via actionpacket code)
                                    client->syncs.forEachRunningSyncContainingNode(n, [&](Sync* s) {
                                        if (toDebrisNode->type == FOLDERNODE)
                                        {
                                            LOG_debug << "Sync - remote folder deletion detected " << n->displayname();
                                        }
                                        else
                                        {
                                            LOG_debug << "Sync - remote file deletion detected " << n->displayname() << " Nhandle: " << LOG_NODEHANDLE(n->nodehandle);
                                        }
                                    });
                                }

                                toDebrisNode->syncdeleted = syncdel;
                                break;
                            }
                        } while ((n = n->parent));
                    }
                }
                else
                {
                    Node *tn = NULL;
                    if (syncdel == SYNCDEL_BIN || syncdel == SYNCDEL_FAILED
                            || !(tn = client->nodeByHandle(client->rootnodes.rubbish)))
                    {
                        LOG_err << "Error moving node to the Rubbish Bin";
                        syncn->syncdeleted = SYNCDEL_NONE;
                        client->toDebris.erase(syncn->todebris_it);
                        syncn->todebris_it = client->toDebris.end();
                    }
                    else
                    {
                        int creqtag = client->reqtag;
                        client->reqtag = syncn->tag;
                        LOG_warn << "Move to Syncdebris failed. Moving to the Rubbish Bin instead.";
                        client->rename(syncn, tn, SYNCDEL_FAILED, pp, nullptr, mCanChangeVault, nullptr);
                        client->reqtag = creqtag;
                    }
                }
            }
        }
        else if(syncop)
        {
            Node *n = client->nodeByHandle(h);
            if(n)
            {
                // After speculative instant completion removal, this is not needed (always sent via actionpacket code)
                Node* prevparent = client->nodeByHandle(pp);
                LOG_debug << "Sync - remote move detected: " << n->displayname() <<
                    " from " << (prevparent ? prevparent->displayname() : "?") <<
                    " to " << (n->parent ? n->parent->displayname() : "?");
            }
        }
#endif
        // Movement of shares and pending shares into Rubbish should remove them
        if (r.wasStrictlyError() && syncdel == SYNCDEL_NONE)
        {
            client->sendevent(99439, "Unexpected move error", 0);
        }
    }
    if (completion) completion(h, r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandDelNode::CommandDelNode(MegaClient* client, NodeHandle th, bool keepversions, int cmdtag, std::function<void(NodeHandle, Error)>&& f, bool canChangeVault)
    : mResultFunction(move(f))
{
    cmd("d");
    notself(client);

    arg("n", (byte*)&th, MegaClient::NODEHANDLE);

    if (keepversions)
    {
        arg("v", 1);
    }

    if (canChangeVault)
    {
        arg("vw", 1);
    }

    h = th;
    tag = cmdtag;
}

bool CommandDelNode::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (mResultFunction)    mResultFunction(h, r.errorOrOK());
        else         client->app->unlink_result(h.as8byte(), r.errorOrOK());
        return true;
    }
    else
    {
        error e = API_OK;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case 'r':
                    if (client->json.enterarray())
                    {
                        if(client->json.isnumeric())
                        {
                            e = (error)client->json.getint();
                        }

                        client->json.leavearray();
                    }
                    break;

                case EOO:
                    if (mResultFunction)    mResultFunction(h, e);
                    else         client->app->unlink_result(h.as8byte(), e);
                    return true;

                default:
                    if (!client->json.storeobject())
                    {
                        if (mResultFunction)    mResultFunction(h, API_EINTERNAL);
                        else         client->app->unlink_result(h.as8byte(), API_EINTERNAL);
                        return false;
                    }
            }
        }
    }
}


CommandDelVersions::CommandDelVersions(MegaClient* client)
{
    cmd("dv");
    tag = client->reqtag;
}

bool CommandDelVersions::procresult(Result r)
{
    client->app->unlinkversions_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandKillSessions::CommandKillSessions(MegaClient* client)
{
    cmd("usr");
    arg("ko", 1); // Request to kill all sessions except the current one

    h = UNDEF;
    tag = client->reqtag;
}

CommandKillSessions::CommandKillSessions(MegaClient* client, handle sessionid)
{
    cmd("usr");
    beginarray("s");
    element(sessionid, MegaClient::USERHANDLE);
    endarray();

    h = sessionid;
    tag = client->reqtag;
}

bool CommandKillSessions::procresult(Result r)
{
    client->app->sessions_killed(h, r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandLogout::CommandLogout(MegaClient *client, Completion completion, bool keepSyncConfigsFile)
  : mCompletion(std::move(completion))
  , mKeepSyncConfigsFile(keepSyncConfigsFile)
{
    cmd("sml");

    batchSeparately = true;

    tag = client->reqtag;
}

bool CommandLogout::procresult(Result r)
{
    assert(r.wasErrorOrOK());
    if (client->loggingout > 0)
    {
        client->loggingout--;
    }
    if(r.wasError(API_OK))
    {
        // We are logged out, but we mustn't call locallogout until we exit this call
        // stack for processing CS batches, as it deletes data currently in use.
        Completion completion = std::move(mCompletion);
        bool keepSyncConfigsFile = mKeepSyncConfigsFile;
        client->mOnCSCompletion = [=](MegaClient* client){
            client->locallogout(true, keepSyncConfigsFile);
            completion(API_OK);
        };
    }
    else
    {
        mCompletion(r.errorOrOK());
    }
    return true;
}

CommandPrelogin::CommandPrelogin(MegaClient* client, const char* email)
{
    cmd("us0");
    arg("user", email);
    batchSeparately = true;  // in case the account is blocked (we need to get a sid so we can issue whyamiblocked)

    this->email = email;
    tag = client->reqtag;
}

bool CommandPrelogin::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->prelogin_result(0, NULL, NULL, r.errorOrOK());
        return true;
    }

    assert(r.hasJsonObject());
    int v = 0;
    string salt;
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'v':
                v = int(client->json.getint());
                break;
            case 's':
                client->json.storeobject(&salt);
                break;
            case EOO:
                if (v == 0)
                {
                    LOG_err << "No version returned";
                    client->app->prelogin_result(0, NULL, NULL, API_EINTERNAL);
                }
                else if (v > 2)
                {
                    LOG_err << "Version of account not supported";
                    client->app->prelogin_result(0, NULL, NULL, API_EINTERNAL);
                }
                else if (v == 2 && !salt.size())
                {
                    LOG_err << "No salt returned";
                    client->app->prelogin_result(0, NULL, NULL, API_EINTERNAL);
                }
                else
                {
                    client->accountversion = v;
                    Base64::atob(salt, client->accountsalt);
                    client->app->prelogin_result(v, &email, &salt, API_OK);
                }
                return true;
            default:
                if (!client->json.storeobject())
                {
                    client->app->prelogin_result(0, NULL, NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

// login request with user e-mail address and user hash
CommandLogin::CommandLogin(MegaClient* client, const char* email, const byte *emailhash, int emailhashsize, const byte *sessionkey, int csessionversion, const char *pin)
{
    cmd("us");
    batchSeparately = true;  // in case the account is blocked (we need to get a sid so we can issue whyamiblocked)

    // are we just performing a session validation?
    checksession = !email;
    sessionversion = csessionversion;

    if (!checksession)
    {
        arg("user", email);
        arg("uh", emailhash, emailhashsize);
        if (pin)
        {
            arg("mfa", pin);
        }
    }
    else
    {
        if (client->sctable && client->dbaccess->currentDbVersion == DbAccess::LEGACY_DB_VERSION)
        {
            LOG_debug << "Requesting a local cache upgrade";
            arg("fa", 1);
        }
    }

    if (sessionkey)
    {
        arg("sek", sessionkey, SymmCipher::KEYLENGTH);
    }

    if (client->cachedscsn != UNDEF)
    {
        arg("sn", (byte*)&client->cachedscsn, sizeof client->cachedscsn);
    }

    string deviceIdHash = client->getDeviceidHash();
    if (!deviceIdHash.empty())
    {
        arg("si", deviceIdHash.c_str());
    }
    else
    {
        client->sendevent(99454, "Device-id not available at login");
    }

    tag = client->reqtag;
}

// process login result
bool CommandLogin::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->login_result(r.errorOrOK());
        return true;
    }

    assert(r.hasJsonObject());
    byte hash[SymmCipher::KEYLENGTH];
    byte sidbuf[AsymmCipher::MAXKEYLENGTH];
    byte privkbuf[AsymmCipher::MAXKEYLENGTH * 2];
    byte sek[SymmCipher::KEYLENGTH];
    int len_k = 0, len_privk = 0, len_csid = 0, len_tsid = 0, len_sek = 0;
    handle me = UNDEF;
    bool fa = false;
    bool ach = false;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'k':
                len_k = client->json.storebinary(hash, sizeof hash);
                break;

            case 'u':
                me = client->json.gethandle(MegaClient::USERHANDLE);
                break;

            case MAKENAMEID3('s', 'e', 'k'):
                len_sek = client->json.storebinary(sek, sizeof sek);
                break;

            case MAKENAMEID4('t', 's', 'i', 'd'):
                len_tsid = client->json.storebinary(sidbuf, sizeof sidbuf);
                break;

            case MAKENAMEID4('c', 's', 'i', 'd'):
                len_csid = client->json.storebinary(sidbuf, sizeof sidbuf);
                break;

            case MAKENAMEID5('p', 'r', 'i', 'v', 'k'):
                len_privk = client->json.storebinary(privkbuf, sizeof privkbuf);
                break;

            case MAKENAMEID2('f', 'a'):
                fa = client->json.getint();
                break;

            case MAKENAMEID3('a', 'c', 'h'):
                ach = client->json.getint();
                break;

            case MAKENAMEID2('s', 'n'):
                if (!client->json.getint())
                {
                    // local state cache continuity rejected: read state from
                    // server instead
                    client->cachedscsn = UNDEF;
                }
                break;

            case EOO:
                if (!checksession)
                {
                    if (ISUNDEF(me) || len_k != sizeof hash)
                    {
                        client->app->login_result(API_EINTERNAL);
                        return true;
                    }

                    // decrypt and set master key
                    client->key.ecb_decrypt(hash);
                    client->key.setkey(hash);
                }
                else
                {
                    if (fa && client->sctable)
                    {
                        client->sctable->remove();
                        client->sctable.reset();
                        client->pendingsccommit = false;
                        client->cachedscsn = UNDEF;
                        client->dbaccess->currentDbVersion = DbAccess::DB_VERSION;

                        client->sendevent(99404, "Local DB upgrade granted", 0);
                    }
                }

                if (len_sek)
                {
                    if (len_sek != SymmCipher::KEYLENGTH)
                    {
                        client->app->login_result(API_EINTERNAL);
                        return true;
                    }

                    if (checksession && sessionversion)
                    {
                        byte k[SymmCipher::KEYLENGTH];
                        memcpy(k, client->key.key, sizeof(k));

                        client->key.setkey(sek);
                        client->key.ecb_decrypt(k);
                        client->key.setkey(k);
                    }
                }

                if (len_tsid)
                {
                    client->sid.assign((const char *)sidbuf, MegaClient::SIDLEN);

                    // account does not have an RSA keypair set: verify
                    // password using symmetric challenge
                    if (!client->checktsid(sidbuf, len_tsid))
                    {
                        LOG_warn << "Error checking tsid";
                        client->app->login_result(API_ENOENT);
                        return true;
                    }

                    // add missing RSA keypair
                    LOG_info << "Generating and adding missing RSA keypair";
                    client->setkeypair();
                }
                else
                {
                    // account has RSA keypair: decrypt server-provided session ID
                    if (len_privk < 256)
                    {
                        if (!checksession)
                        {
                            client->app->login_result(API_EINTERNAL);
                            return true;
                        }
                        else if (!client->ephemeralSessionPlusPlus)
                        {
                            // logging in with tsid to an account without a RSA keypair
                            LOG_info << "Generating and adding missing RSA keypair";
                            client->setkeypair();
                        }
                    }
                    else
                    {
                        // decrypt and set private key
                        client->key.ecb_decrypt(privkbuf, len_privk);
                        client->mPrivKey.resize(AsymmCipher::MAXKEYLENGTH * 2);
                        client->mPrivKey.resize(Base64::btoa(privkbuf, len_privk, (char *)client->mPrivKey.data()));

                        if (!client->asymkey.setkey(AsymmCipher::PRIVKEY, privkbuf, len_privk))
                        {
                            LOG_warn << "Error checking private key";
                            client->app->login_result(API_ENOENT);
                            return true;
                        }
                    }

                    if (!checksession)
                    {
                        if (len_csid < 32)
                        {
                            client->app->login_result(API_EINTERNAL);
                            return true;
                        }

                        byte buf[sizeof me];

                        // decrypt and set session ID for subsequent API communication
                        if (!client->asymkey.decrypt(sidbuf, len_csid, sidbuf, MegaClient::SIDLEN)
                                // additionally, check that the user's handle included in the session matches the own user's handle (me)
                                || (Base64::atob((char*)sidbuf + SymmCipher::KEYLENGTH, buf, sizeof buf) != sizeof buf)
                                || (me != MemAccess::get<handle>((const char*)buf)))
                        {
                            client->app->login_result(API_EINTERNAL);
                            return true;
                        }

                        client->sid.assign((const char *)sidbuf, MegaClient::SIDLEN);
                    }
                }

                client->me = me;
                client->uid = Base64Str<MegaClient::USERHANDLE>(client->me);
                client->achievements_enabled = ach;
                // Force to create own user
                client->finduser(me, 1);

                if (len_sek)
                {
                    client->sessionkey.assign((const char *)sek, sizeof(sek));
                }

                client->openStatusTable(true);
                client->app->login_result(API_OK);
                client->getaccountdetails(std::make_shared<AccountDetails>(), false, false, true, false, false, false);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->login_result(API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandShareKeyUpdate::CommandShareKeyUpdate(MegaClient*, handle sh, const char* uid, const byte* key, int len)
{
    cmd("k");
    beginarray("sr");

    element(sh, MegaClient::NODEHANDLE);
    element(uid);
    element(key, len);

    endarray();
}

CommandShareKeyUpdate::CommandShareKeyUpdate(MegaClient* client, handle_vector* v)
{
    Node* n;
    byte sharekey[SymmCipher::KEYLENGTH];

    cmd("k");
    beginarray("sr");

    for (size_t i = v->size(); i--;)
    {
        handle h = (*v)[i];

        if ((n = client->nodebyhandle(h)) && n->sharekey)
        {
            client->key.ecb_encrypt(n->sharekey->key, sharekey, SymmCipher::KEYLENGTH);

            element(h, MegaClient::NODEHANDLE);
            element(client->me, MegaClient::USERHANDLE);
            element(sharekey, SymmCipher::KEYLENGTH);
        }
    }

    endarray();
}

// add/remove share; include node share keys if new share
CommandSetShare::CommandSetShare(MegaClient* client, Node* n, User* u, accesslevel_t a, bool newshare, const char* msg, bool writable, const char* personal_representation, int ctag, std::function<void(Error, bool writable)> f)
{
    byte auth[SymmCipher::BLOCKSIZE];
    byte key[SymmCipher::KEYLENGTH];
    byte asymmkey[AsymmCipher::MAXKEYLENGTH];
    int t = 0;

    tag = ctag;

    sh = n->nodehandle;
    user = u;
    access = a;
    mWritable = writable;

    completion = move(f);
    assert(completion);

    cmd("s2");
    arg("n", (byte*)&sh, MegaClient::NODEHANDLE);

    // Only for inviting non-contacts
    if (personal_representation && personal_representation[0])
    {
        this->personal_representation = personal_representation;
        arg("e", personal_representation);
    }

    if (msg && msg[0])
    {
        this->msg = msg;
        arg("msg", msg);
    }

    if (a != ACCESS_UNKNOWN)
    {
        // securely store/transmit share key
        // by creating a symmetrically (for the sharer) and an asymmetrically
        // (for the sharee) encrypted version
        memcpy(key, n->sharekey->key, sizeof key);
        memcpy(asymmkey, key, sizeof key);

        client->key.ecb_encrypt(key);
        arg("ok", key, sizeof key);

        if (u && u->pubk.isvalid())
        {
            t = u->pubk.encrypt(client->rng, asymmkey, SymmCipher::KEYLENGTH, asymmkey, sizeof asymmkey);
        }

        // outgoing handle authentication
        client->handleauth(sh, auth);
        arg("ha", auth, sizeof auth);
    }

    beginarray("s");
    beginobject();

    arg("u", u ? ((u->show == VISIBLE) ? u->uid.c_str() : u->email.c_str()) : MegaClient::EXPORTEDLINK);
    // if the email is registered, the pubk request has returned the userhandle -->
    // sending the userhandle instead of the email makes the API to assume the user is already a contact

    if (a != ACCESS_UNKNOWN)
    {
        arg("r", a);

        if (u && u->pubk.isvalid() && t)
        {
            arg("k", asymmkey, t);
        }
    }

    endobject();
    endarray();

    // only for a fresh share: add cr element with all node keys encrypted to
    // the share key
    if (newshare)
    {
        // the new share's nodekeys for this user: generate node list
        TreeProcShareKeys tpsk(n);
        client->proctree(n, &tpsk);
        tpsk.get(this);
    }
}

// process user element (email/handle pairs)
bool CommandSetShare::procuserresult(MegaClient* client)
{
    while (client->json.enterobject())
    {
        handle uh = UNDEF;
        const char* m = NULL;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case 'u':
                    uh = client->json.gethandle(MegaClient::USERHANDLE);
                    break;

                case 'm':
                    m = client->json.getvalue();
                    break;

                case EOO:
                    if (!ISUNDEF(uh) && m)
                    {
                        client->mapuser(uh, m);
                    }
                    return true;

                default:
                    if (!client->json.storeobject())
                    {
                        return false;
                    }
            }
        }
    }

    return false;
}

// process result of share addition/modification
bool CommandSetShare::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        completion(r.errorOrOK(), mWritable);
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID2('o', 'k'):  // an owner key response will only
                                         // occur if the same share was created
                                         // concurrently with a different key
            {
                byte key[SymmCipher::KEYLENGTH + 1];
                if (client->json.storebinary(key, sizeof key + 1) == SymmCipher::KEYLENGTH)
                {
                    Node* n;

                    if ((n = client->nodebyhandle(sh)) && n->sharekey)
                    {
                        client->key.ecb_decrypt(key);
                        n->sharekey->setkey(key);

                        // repeat attempt with corrected share key
                        client->reqs.add(new CommandSetShare(client, n, user, access, 0, msg.c_str(), mWritable, personal_representation.c_str(),
                                         tag, move(completion)));
                        return false;
                    }
                }
                break;
            }

            case 'u':   // user/handle confirmation
                if (client->json.enterarray())
                {
                    while (procuserresult(client))
                    {}
                    client->json.leavearray();
                }
                break;

            case 'r':
                if (client->json.enterarray())
                {
                    while (client->json.isnumeric())
                    {
                        // intermediate result updates, not final completion
                        // we used to call share_result but it wasn't used
                        client->json.getint();
                    }

                    client->json.leavearray();
                }
                break;

            case MAKENAMEID3('s', 'n', 'k'):
                client->procsnk(&client->json);
                break;

            case MAKENAMEID3('s', 'u', 'k'):
                client->procsuk(&client->json);
                break;

            case MAKENAMEID2('c', 'r'):
                client->proccr(&client->json);
                break;

            case EOO:
                completion(API_OK, mWritable);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    return false;
                }
        }
    }
}

CommandSetPendingContact::CommandSetPendingContact(MegaClient* client, const char* temail, opcactions_t action, const char* msg, const char* oemail, handle contactLink, Completion completion)
{
    cmd("upc");

    if (oemail != NULL)
    {
        arg("e", oemail);
    }

    arg("u", temail);
    switch (action)
    {
        case OPCA_DELETE:
            arg("aa", "d");
            break;
        case OPCA_REMIND:
            arg("aa", "r");
            break;
        case OPCA_ADD:
            arg("aa", "a");
            if (!ISUNDEF(contactLink))
            {
                arg("cl", (byte*)&contactLink, MegaClient::CONTACTLINKHANDLE);
            }
            break;
    }

    if (msg != NULL)
    {
        arg("msg", msg);
    }

    if (action != OPCA_REMIND)  // for reminders, need the actionpacket to update `uts`
    {
        notself(client);
    }

    tag = client->reqtag;
    this->action = action;
    this->temail = temail;

    // Assume we've been passed a completion function.
    mCompletion = std::move(completion);
}

bool CommandSetPendingContact::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        handle pcrhandle = UNDEF;
        if (r.wasError(API_OK)) // response for delete & remind actions is always numeric
        {
            // find the PCR by email
            PendingContactRequest *pcr = NULL;
            for (handlepcr_map::iterator it = client->pcrindex.begin();
                 it != client->pcrindex.end(); it++)
            {
                if (it->second->targetemail == temail)
                {
                    pcr = it->second.get();
                    pcrhandle = pcr->id;
                    break;
                }
            }

            if (!pcr)
            {
                LOG_err << "Reminded/deleted PCR not found";
            }
            else if (action == OPCA_DELETE)
            {
                pcr->changed.deleted = true;
                client->notifypcr(pcr);

                // remove pending shares related to the deleted PCR
                Node *n;
                for (node_map::iterator it = client->nodes.begin(); it != client->nodes.end(); it++)
                {
                    n = it->second;
                    if (n->pendingshares && n->pendingshares->find(pcr->id) != n->pendingshares->end())
                    {
                        client->newshares.push_back(
                                    new NewShare(n->nodehandle, 1, n->owner, ACCESS_UNKNOWN,
                                                 0, NULL, NULL, pcr->id, false));
                    }
                }

                client->mergenewshares(1);
            }
        }

        doComplete(pcrhandle, r.errorOrOK(), this->action);
        return true;
    }

    // if the PCR has been added, the response contains full details
    handle p = UNDEF;
    m_time_t ts = 0;
    m_time_t uts = 0;
    const char *eValue = NULL;
    const char *m = NULL;
    const char *msg = NULL;
    PendingContactRequest *pcr = NULL;
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'p':
                p = client->json.gethandle(MegaClient::PCRHANDLE);
                break;
            case 'm':
                m = client->json.getvalue();
                break;
            case 'e':
                eValue = client->json.getvalue();
                break;
            case MAKENAMEID3('m', 's', 'g'):
                msg = client->json.getvalue();
                break;
            case MAKENAMEID2('t', 's'):
                ts = client->json.getint();
                break;
            case MAKENAMEID3('u', 't', 's'):
                uts = client->json.getint();
                break;
            case EOO:
                if (ISUNDEF(p))
                {
                    LOG_err << "Error in CommandSetPendingContact. Undefined handle";
                    doComplete(UNDEF, API_EINTERNAL, this->action);
                    return true;
                }

                if (action != OPCA_ADD || !eValue || !m || ts == 0 || uts == 0)
                {
                    LOG_err << "Error in CommandSetPendingContact. Wrong parameters";
                    doComplete(UNDEF, API_EINTERNAL, this->action);
                    return true;
                }

                pcr = new PendingContactRequest(p, eValue, m, ts, uts, msg, true);
                client->mappcr(p, unique_ptr<PendingContactRequest>(pcr));

                client->notifypcr(pcr);
                doComplete(p, API_OK, this->action);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Error in CommandSetPendingContact. Parse error";
                    doComplete(UNDEF, API_EINTERNAL, this->action);
                    return false;
                }
        }
    }
}

void CommandSetPendingContact::doComplete(handle handle, error result, opcactions_t actions)
{
    if (!mCompletion)
        return client->app->setpcr_result(handle, result, actions);

    mCompletion(handle, result, actions);
}

CommandUpdatePendingContact::CommandUpdatePendingContact(MegaClient* client, handle p, ipcactions_t action, Completion completion)
{
    cmd("upca");

    arg("p", (byte*)&p, MegaClient::PCRHANDLE);
    switch (action)
    {
        case IPCA_ACCEPT:
            arg("aa", "a");
            break;
        case IPCA_DENY:
            arg("aa", "d");
            break;
        case IPCA_IGNORE:
        default:
            arg("aa", "i");
            break;
    }

    tag = client->reqtag;
    this->action = action;

    // Assume we've been provided a completion function.
    mCompletion = std::move(completion);
}

bool CommandUpdatePendingContact::procresult(Result r)
{
    doComplete(r.errorOrOK(), this->action);

    return r.wasErrorOrOK();
}


void CommandUpdatePendingContact::doComplete(error result, ipcactions_t actions)
{
    if (!mCompletion)
        return client->app->updatepcr_result(result, actions);

    mCompletion(result, actions);
}

CommandEnumerateQuotaItems::CommandEnumerateQuotaItems(MegaClient* client)
{
    cmd("utqa");
    arg("nf", 3);
    arg("b", 1);    // support for Business accounts
    arg("p", 1);    // support for Pro Flexi
    tag = client->reqtag;
}

bool CommandEnumerateQuotaItems::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->enumeratequotaitems_result(r.errorOrOK());
        return true;
    }

    string currency; // common for all plans, populated from `l` object

    while (client->json.enterobject())
    {
        handle product = UNDEF;
        int prolevel = -1, gbstorage = -1, gbtransfer = -1, months = -1, type = -1;
        unsigned amount = 0, amountMonth = 0, localPrice = 0;
        string description;
        string ios_id;
        string android_id;

        unique_ptr<BusinessPlan> bizPlan;
        unique_ptr<CurrencyData> currencyData;

        bool finished = false;
        bool readingL = false;
        const char* buf = nullptr;
        while (!finished)
        {
            buf = nullptr;

            switch (client->json.getnameid())
            {
                case MAKENAMEID1('l'):  // currency localization
                {
                    if (!client->json.enterobject())
                    {
                        LOG_err << "Failed to parse Enumerate-quota-items response, `l` object";
                        client->app->enumeratequotaitems_result(API_EINTERNAL);
                        return false;
                    }

                    currencyData = mega::make_unique<CurrencyData>();
                    readingL = true;

                    while (!finished)
                    {
                        buf = nullptr;

                        switch(client->json.getnameid())
                        {
                            case MAKENAMEID1('c'):  // currency, ie. EUR
                                buf = client->json.getvalue();
                                JSON::copystring(&currencyData->currencyName, buf);
                                currency = currencyData->currencyName;
                                break;
                            case MAKENAMEID2('c', 's'): // currency symbol, ie. €
                                buf = client->json.getvalue();
                                JSON::copystring(&currencyData->currencySymbol, buf);
                                break;
                            case MAKENAMEID2('l', 'c'):  // local currency, ie. NZD
                                buf = client->json.getvalue();
                                JSON::copystring(&currencyData->localCurrencyName, buf);
                                break;
                            case MAKENAMEID3('l', 'c', 's'):    // local currency symbol, ie. $
                                buf = client->json.getvalue();
                                JSON::copystring(&currencyData->localCurrencySymbol, buf);
                                break;
                            case EOO:
                                // sanity checks for received data
                                if (currencyData->currencyName.empty() || currencyData->currencySymbol.empty())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `l` data";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return true;
                                }

                                finished = true;    // exits from the outer loop too
                                client->json.leaveobject(); // 'l' object
                                break;
                            default:
                                if (!client->json.storeobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, store `l` data";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }
                                break;
                        }
                    }
                    break;
                }
                case MAKENAMEID2('i', 't'): // 0 -> for all Pro level plans; 1 -> for Business plan
                    type = static_cast<int>(client->json.getint());
                    break;
//                case MAKENAMEID2('i', 'b'): // for "it":1 (business plans), 0 -> Pro Flexi; 1 -> Business plan
//                    {
//                        bool isProFlexi = client->json.getbool();
//                    }
//                    break;
                case MAKENAMEID2('i', 'd'):
                    product = client->json.gethandle(8);
                    break;
                case MAKENAMEID2('a', 'l'):
                    prolevel = static_cast<int>(client->json.getint());
                    break;
                case 's':
                    gbstorage = static_cast<int>(client->json.getint());
                    break;
                case 't':
                    gbtransfer = static_cast<int>(client->json.getint());
                    break;
                case 'm':
                    months = static_cast<int>(client->json.getint());
                    break;
                case 'p':   // price (in cents)
                    amount = static_cast<unsigned>(client->json.getint());
                    break;
                case 'd':
                    buf = client->json.getvalue();
                    JSON::copystring(&description, buf);
                    break;
                case MAKENAMEID3('i', 'o', 's'):
                    buf = client->json.getvalue();
                    JSON::copystring(&ios_id, buf);
                    break;
                case MAKENAMEID6('g', 'o', 'o', 'g', 'l', 'e'):
                    buf = client->json.getvalue();
                    JSON::copystring(&android_id, buf);
                    break;
                case MAKENAMEID3('m', 'b', 'p'):    // monthly price (in cents)
                    amountMonth = static_cast<unsigned>(client->json.getint());
                    break;
                case MAKENAMEID2('l', 'p'): // local price (in cents)
                    localPrice = static_cast<unsigned>(client->json.getint());
                    break;
                case MAKENAMEID2('b', 'd'): // BusinessPlan
                {
                    if (!client->json.enterobject())
                    {
                        LOG_err << "Failed to parse Enumerate-quota-items response, `bd` object";
                        client->app->enumeratequotaitems_result(API_EINTERNAL);
                        return false;
                    }

                    bizPlan = mega::make_unique<BusinessPlan>();

                    bool readingBd = true;
                    while (readingBd)
                    {
                        switch (client->json.getnameid())
                        {
                            case MAKENAMEID2('b', 'a'): // base (-1 means unlimited storage or transfer)
                            {
                                if (!client->json.enterobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `ba` object";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }

                                bool readingBa = true;
                                while (readingBa)
                                {
                                    switch (client->json.getnameid())
                                    {
                                        case 's':
                                            bizPlan->gbStoragePerUser = static_cast<int>(client->json.getint());
                                            break;
                                        case 't':
                                            bizPlan->gbTransferPerUser = static_cast<int>(client->json.getint());
                                            break;
                                        case EOO:
                                            readingBa = false;
                                            break;
                                        default:
                                            if (!client->json.storeobject())
                                            {
                                                LOG_err << "Failed to parse Enumerate-quota-items response, `ba` data";
                                                client->app->enumeratequotaitems_result(API_EINTERNAL);
                                                return false;
                                            }
                                            break;
                                    }
                                }
                                client->json.leaveobject();
                                break;
                            }
                            case MAKENAMEID2('u', 's'):   // price per user
                            {
                                if (!client->json.enterobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `us` object";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }

                                bool readingUs = true;
                                while (readingUs)
                                {
                                    switch (client->json.getnameid())
                                    {
                                        case 'p':
                                            bizPlan->pricePerUser = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case MAKENAMEID2('l', 'p'):
                                            bizPlan->localPricePerUser = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case EOO:
                                            readingUs = false;
                                            break;
                                        default:
                                            if (!client->json.storeobject())
                                            {
                                                LOG_err << "Failed to parse Enumerate-quota-items response, `us` data";
                                                client->app->enumeratequotaitems_result(API_EINTERNAL);
                                                return false;
                                            }
                                            break;
                                    }
                                }
                                client->json.leaveobject();
                                break;
                            }
                            case MAKENAMEID3('s', 't', 'o'):   // storage block
                            {
                                if (!client->json.enterobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `sto` object";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }

                                bool readingSto = true;
                                while (readingSto)
                                {
                                    switch (client->json.getnameid())
                                    {
                                        case 's':
                                            bizPlan->gbPerStorage = static_cast<int>(client->json.getint());
                                            break;
                                        case 'p':
                                            bizPlan->pricePerStorage = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case MAKENAMEID2('l', 'p'):
                                            bizPlan->localPricePerStorage = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case EOO:
                                            readingSto = false;
                                            break;
                                        default:
                                            if (!client->json.storeobject())
                                            {
                                                LOG_err << "Failed to parse Enumerate-quota-items response, `sto` data";
                                                client->app->enumeratequotaitems_result(API_EINTERNAL);
                                                return false;
                                            }
                                            break;
                                    }
                                }
                                client->json.leaveobject();
                                break;
                            }
                            case MAKENAMEID4('t', 'r', 'n', 's'):   // transfer block
                            {
                                if (!client->json.enterobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `trns` object";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }

                                bool readingTrns = true;
                                while (readingTrns)
                                {
                                    switch (client->json.getnameid())
                                    {
                                        case 't':
                                            bizPlan->gbPerTransfer = static_cast<int>(client->json.getint());
                                            break;
                                        case 'p':
                                            bizPlan->pricePerTransfer = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case MAKENAMEID2('l', 'p'):
                                            bizPlan->localPricePerTransfer = static_cast<unsigned>(client->json.getint());
                                            break;
                                        case EOO:
                                            readingTrns = false;
                                            break;
                                        default:
                                            if (!client->json.storeobject())
                                            {
                                                LOG_err << "Failed to parse Enumerate-quota-items response, `sto` data";
                                                client->app->enumeratequotaitems_result(API_EINTERNAL);
                                                return false;
                                            }
                                            break;
                                    }
                                }
                                client->json.leaveobject();
                                break;
                            }
                            case MAKENAMEID4('m', 'i', 'n', 'u'):   // minimum number of user required to purchase
                                bizPlan->minUsers = static_cast<int>(client->json.getint());
                                break;
                            case EOO:
                                readingBd = false;
                                break;
                            default:
                                if (!client->json.storeobject())
                                {
                                    LOG_err << "Failed to parse Enumerate-quota-items response, `bd` object";
                                    client->app->enumeratequotaitems_result(API_EINTERNAL);
                                    return false;
                                }
                                break;
                        }
                    }
                    client->json.leaveobject();
                    break;
                }
                case EOO:
                    if (type < 0
                            || ISUNDEF(product)
                            || (prolevel < 0)
                            || (months < 0)
                            || currency.empty()
                            || description.empty()
                            // only available for Pro plans, not for Business
                            || (!type && gbstorage < 0)
                            || (!type && gbtransfer < 0)
                            || (!type && !amount)
                            || (!type && !amountMonth)
                            || (!type && ios_id.empty())
                            || (!type && android_id.empty())
                            // only available for Business plan(s)
                            || (type == 1 && !bizPlan))
                    {
                        client->app->enumeratequotaitems_result(API_EINTERNAL);
                        return true;
                    }

                    finished = true;
                    break;
                default:
                    if (!client->json.storeobject())
                    {
                        LOG_err << "Failed to parse Enumerate-quota-items response";
                        client->app->enumeratequotaitems_result(API_EINTERNAL);
                        return false;
                    }
                    break;
            }
        }   // end while(!finished)

        client->json.leaveobject();

        if (readingL)
        {
            // just read currency data, keep reading objects for each pro/business plan
            readingL = false;
            client->app->enumeratequotaitems_result(move(currencyData));
            continue;
        }
        else
        {
            client->app->enumeratequotaitems_result(type, product, prolevel, gbstorage,
                                                    gbtransfer, months, amount, amountMonth, localPrice,
                                                    description.c_str(), ios_id.c_str(), android_id.c_str(),
                                                    move(bizPlan));
        }
    }

    client->app->enumeratequotaitems_result(API_OK);
    return true;
}

CommandPurchaseAddItem::CommandPurchaseAddItem(MegaClient* client, int itemclass,
                                               handle item, unsigned price,
                                               const char* currency, unsigned /*tax*/,
                                               const char* /*country*/, handle lph,
                                               int phtype, int64_t ts)
{
    string sprice;
    sprice.resize(128);
    sprintf((char *)sprice.data(), "%.2f", price/100.0);
    replace( sprice.begin(), sprice.end(), ',', '.');
    cmd("uts");
    arg("it", itemclass);
    arg("si", (byte*)&item, 8);
    arg("p", sprice.c_str());
    arg("c", currency);
    if (!ISUNDEF(lph))
    {
        if (phtype == 0) // legacy mode
        {
            arg("aff", (byte*)&lph, MegaClient::NODEHANDLE);
        }
        else
        {
            beginobject("aff");
            arg("id", (byte*)&lph, MegaClient::NODEHANDLE);
            arg("ts", ts);
            arg("t", phtype);   // 1=affiliate id, 2=file/folder link, 3=chat link, 4=contact link
            endobject();
        }
    }

    tag = client->reqtag;

    //TODO: Complete this (tax? country?)
}

bool CommandPurchaseAddItem::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->additem_result(r.errorOrOK());
        return true;
    }

    handle item = client->json.gethandle(8);
    if (item != UNDEF)
    {
        client->purchase_basket.push_back(item);
        client->app->additem_result(API_OK);
        return true;
    }
    else
    {
        client->json.storeobject();
        client->app->additem_result(API_EINTERNAL);
        return false;
    }
}

CommandPurchaseCheckout::CommandPurchaseCheckout(MegaClient* client, int gateway)
{
    cmd("utc");

    beginarray("s");
    for (handle_vector::iterator it = client->purchase_basket.begin(); it != client->purchase_basket.end(); it++)
    {
        element((byte*)&*it, sizeof(handle));
    }

    endarray();

    arg("m", gateway);

    // empty basket
    client->purchase_begin();

    tag = client->reqtag;
}

bool CommandPurchaseCheckout::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->checkout_result(NULL, r.errorOrOK());
        return true;
    }

    //Expected response: "EUR":{"res":X,"code":Y}}
    client->json.getnameid();
    if (!client->json.enterobject())
    {
        LOG_err << "Parse error (CommandPurchaseCheckout)";
        client->app->checkout_result(NULL, API_EINTERNAL);
        return false;
    }

    string errortype;
    Error e;
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID3('r', 'e', 's'):
                if (client->json.isnumeric())
                {
                    e = (error)client->json.getint();
                }
                else
                {
                    client->json.storeobject(&errortype);
                    if (errortype == "S")
                    {
                        errortype.clear();
                        e = API_OK;
                    }
                }
                break;

            case MAKENAMEID4('c', 'o', 'd', 'e'):
                if (client->json.isnumeric())
                {
                    e = (error)client->json.getint();
                }
                else
                {
                    LOG_err << "Parse error in CommandPurchaseCheckout (code)";
                }
                break;
            case EOO:
                client->json.leaveobject();
                if (!errortype.size() || errortype == "FI" || e == API_OK)
                {
                    client->app->checkout_result(NULL, e);
                }
                else
                {
                    client->app->checkout_result(errortype.c_str(), e);
                }
                return true;
            default:
                if (!client->json.storeobject())
                {
                    client->app->checkout_result(NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandRemoveContact::CommandRemoveContact(MegaClient* client, const char* m, visibility_t show, Completion completion)
{
    this->email = m ? m : "";
    this->v = show;

    cmd("ur2");
    arg("u", m);
    arg("l", (int)show);

    tag = client->reqtag;

    // Assume we've been given a completion function.
    mCompletion = std::move(completion);
}

bool CommandRemoveContact::procresult(Result r)
{
    assert(r.hasJsonObject() || r.wasStrictlyError());

    if (r.hasJsonObject())
    {
        // the object contains (userhandle + email string) - caller will leaveobject() automatically

        if (User *u = client->finduser(email.c_str()))
        {
            u->show = v;
        }

        doComplete(API_OK);
        return true;
    }

    doComplete(r.errorOrOK());
    return r.wasErrorOrOK();
}

void CommandRemoveContact::doComplete(error result)
{
    if (!mCompletion)
        return client->app->removecontact_result(result);

    mCompletion(result);
}

CommandPutMultipleUAVer::CommandPutMultipleUAVer(MegaClient *client, const userattr_map *attrs, int ctag)
{
    this->attrs = *attrs;

    cmd("upv");

    for (userattr_map::const_iterator it = attrs->begin(); it != attrs->end(); it++)
    {
        attr_t type = it->first;

        beginarray(User::attr2string(type).c_str());

        element((const byte *) it->second.data(), int(it->second.size()));

        const string *attrv = client->ownuser()->getattrversion(type);
        if (attrv)
        {
            element(attrv->c_str());
        }

        endarray();
    }

    tag = ctag;
}

bool CommandPutMultipleUAVer::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->sendevent(99419, "Error attaching keys", 0);

        client->app->putua_result(r.errorOrOK());
        return true;
    }

    User *u = client->ownuser();
    for(;;)   // while there are more attrs to read...
    {
        const char* ptr;
        const char* end;

        if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
        {
            break;
        }
        attr_t type = User::string2attr(string(ptr, (end-ptr)).c_str());

        if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
        {
            client->app->putua_result(API_EINTERNAL);
            return false;
        }
        string version = string(ptr, (end-ptr));

        userattr_map::iterator it = this->attrs.find(type);
        if (type == ATTR_UNKNOWN || version.empty() || (it == this->attrs.end()))
        {
            LOG_err << "Error in CommandPutUA. Undefined attribute or version";
            client->app->putua_result(API_EINTERNAL);
            return false;
        }
        else
        {
            u->setattr(type, &it->second, &version);
            u->setTag(tag ? tag : -1);

            if (type == ATTR_KEYRING)
            {
                TLVstore *tlvRecords = TLVstore::containerToTLVrecords(&attrs[type], &client->key);
                if (tlvRecords)
                {
                    string prEd255;
                    if (tlvRecords->get(EdDSA::TLV_KEY, prEd255) && prEd255.size() == EdDSA::SEED_KEY_LENGTH)
                    {
                        client->signkey = new EdDSA(client->rng, (unsigned char *) prEd255.data());
                    }

                    string prCu255;
                    if (tlvRecords->get(ECDH::TLV_KEY, prCu255) && prCu255.size() == ECDH::PRIVATE_KEY_LENGTH)
                    {
                        client->chatkey = new ECDH((unsigned char *) prCu255.data());
                    }

                    if (!client->chatkey || !client->chatkey->initializationOK ||
                            !client->signkey || !client->signkey->initializationOK)
                    {
                        client->resetKeyring();
                        client->sendevent(99418, "Failed to load attached keys", 0);
                    }
                    else
                    {
                        client->sendevent(99420, "Signing and chat keys attached OK", 0);
                    }

                    delete tlvRecords;
                }
                else
                {
                    LOG_warn << "Failed to decrypt keyring after putua";
                }
            }
            else if (User::isAuthring(type))
            {
                client->mAuthRings.erase(type);
                const std::unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&attrs[type], &client->key));
                if (tlvRecords)
                {
                    client->mAuthRings.emplace(type, AuthRing(type, *tlvRecords));
                }
                else
                {
                    LOG_err << "Failed to decrypt keyring after putua";
                }
            }
        }
    }

    client->notifyuser(u);
    client->app->putua_result(API_OK);
    return true;
}


CommandPutUAVer::CommandPutUAVer(MegaClient* client, attr_t at, const byte* av, unsigned avl, int ctag,
                                 std::function<void(Error)> completion)
{
    this->at = at;
    this->av.assign((const char*)av, avl);

    mCompletion = completion ? move(completion) :
        [this](Error e) {
            this->client->app->putua_result(e);
        };

    cmd("upv");

    beginarray(User::attr2string(at).c_str());

    // if removing avatar, do not Base64 encode the attribute value
    if (at == ATTR_AVATAR && !strcmp((const char *)av, "none"))
    {
        element((const char*)av);
    }
    else
    {
        element(av, avl);
    }

    const string *attrv = client->ownuser()->getattrversion(at);
    if (client->ownuser()->isattrvalid(at) && attrv)
    {
        element(attrv->c_str());
    }

    endarray();

    tag = ctag;
}

bool CommandPutUAVer::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_EEXPIRED))
        {
            User *u = client->ownuser();
            u->invalidateattr(at);
        }

        mCompletion(r.errorOrOK());
    }
    else
    {
        const char* ptr;
        const char* end;

        if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
        {
            mCompletion(API_EINTERNAL);
            return false;
        }
        attr_t at = User::string2attr(string(ptr, (end-ptr)).c_str());

        if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
        {
            mCompletion(API_EINTERNAL);
            return false;
        }
        string v = string(ptr, (end-ptr));

        if (at == ATTR_UNKNOWN || v.empty() || (this->at != at))
        {
            LOG_err << "Error in CommandPutUA. Undefined attribute or version";
            mCompletion(API_EINTERNAL);
            return false;
        }
        else
        {
            User *u = client->ownuser();
            u->setattr(at, &av, &v);
            u->setTag(tag ? tag : -1);

            if (User::isAuthring(at))
            {
                client->mAuthRings.erase(at);
                const std::unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&av, &client->key));
                if (tlvRecords)
                {
                    client->mAuthRings.emplace(at, AuthRing(at, *tlvRecords));
                }
                else
                {
                    LOG_err << "Failed to decrypt " << User::attr2string(at) << " after putua";
                }
            }
            else if (at == ATTR_UNSHAREABLE_KEY)
            {
                LOG_info << "Unshareable key successfully created";
                client->unshareablekey.swap(av);
            }
            else if (at == ATTR_JSON_SYNC_CONFIG_DATA)
            {
                LOG_info << "JSON config data successfully created.";
            }

            client->notifyuser(u);
            mCompletion(API_OK);
        }
    }
    return true;
}


CommandPutUA::CommandPutUA(MegaClient* /*client*/, attr_t at, const byte* av, unsigned avl, int ctag, handle lph, int phtype, int64_t ts,
                           std::function<void(Error)> completion)
{
    this->at = at;
    this->av.assign((const char*)av, avl);

    mCompletion = completion ? move(completion) :
                  [this](Error e){
                        client->app->putua_result(e);
                  };

    cmd("up");

    string an = User::attr2string(at);

    // if removing avatar, do not Base64 encode the attribute value
    if (at == ATTR_AVATAR && !strcmp((const char *)av, "none"))
    {
        arg(an.c_str(),(const char *)av, avl);
    }
    else
    {
        arg(an.c_str(), av, avl);
    }

    if (!ISUNDEF(lph))
    {
        beginobject("aff");
        arg("id", (byte*)&lph, MegaClient::NODEHANDLE);
        arg("ts", ts);
        arg("t", phtype);   // 1=affiliate id, 2=file/folder link, 3=chat link, 4=contact link
        endobject();
    }

    tag = ctag;
}

bool CommandPutUA::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        mCompletion(r.errorOrOK());
    }
    else
    {
        client->json.storeobject(); // [<uh>]

        User *u = client->ownuser();
        assert(u);
        if (!u)
        {
            LOG_err << "Own user not found when attempting to set user attributes";
            mCompletion(API_EACCESS);
            return true;
        }
        u->setattr(at, &av, NULL);
        u->setTag(tag ? tag : -1);
        client->notifyuser(u);

        if (at == ATTR_DISABLE_VERSIONS)
        {
            client->versions_disabled = (av == "1");
            if (client->versions_disabled)
            {
                LOG_info << "File versioning is disabled";
            }
            else
            {
                LOG_info << "File versioning is enabled";
            }
        }
        else if (at == ATTR_NO_CALLKIT)
        {
            LOG_info << "CallKit is " << ((av == "1") ? "disabled" : "enabled");
        }

        mCompletion(API_OK);
    }

    return true;
}

CommandGetUA::CommandGetUA(MegaClient* /*client*/, const char* uid, attr_t at, const char* ph, int ctag,
                           CompletionErr completionErr, CompletionBytes completionBytes, CompletionTLV compltionTLV)
{
    this->uid = uid;
    this->at = at;
    this->ph = ph ? string(ph) : "";

    mCompletionErr = completionErr ? move(completionErr) :
        [this](error e) {
            client->app->getua_result(e);
        };

    mCompletionBytes = completionBytes ? move(completionBytes) :
        [this](byte* b, unsigned l, attr_t e) {
            client->app->getua_result(b, l, e);
        };

    mCompletionTLV = compltionTLV ? move(compltionTLV) :
        [this](TLVstore* t, attr_t e) {
            client->app->getua_result(t, e);
        };

    if (ph && ph[0])
    {
        cmd("mcuga");
        arg("ph", ph);
    }
    else
    {
        cmd("uga");
    }

    arg("u", uid);
    arg("ua", User::attr2string(at).c_str());
    arg("v", 1);
    tag = ctag;
}

bool CommandGetUA::procresult(Result r)
{
    User *u = client->finduser(uid.c_str());

    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_ENOENT) && u)
        {
            u->removeattr(at);
        }

        mCompletionErr(r.errorOrOK());

        if (isFromChatPreview())    // if `mcuga` was sent, no need to do anything else
        {
            return true;
        }

        if (u && u->userhandle == client->me && !r.wasError(API_EBLOCKED))
        {
            if (client->fetchingkeys && at == ATTR_SIG_RSA_PUBK)
            {
                client->initializekeys(); // we have now all the required data
            }

            if (r.wasError(API_ENOENT) && User::isAuthring(at))
            {
                // authring not created yet, will do it upon retrieval of public keys
                client->mAuthRings.erase(at);
                client->mAuthRings.emplace(at, AuthRing(at, TLVstore()));

                if (client->mFetchingAuthrings && client->mAuthRings.size() == 3)
                {
                    client->mFetchingAuthrings = false;
                    client->fetchContactsKeys();
                }
            }
        }

        // if the attr does not exist, initialize it
        if (at == ATTR_DISABLE_VERSIONS && r.wasError(API_ENOENT))
        {
            LOG_info << "File versioning is enabled";
            client->versions_disabled = false;
        }
        else if (at == ATTR_NO_CALLKIT && r.wasError(API_ENOENT))
        {
            LOG_info << "CallKit is enabled";
        }

        return true;
    }
    else
    {
        const char* ptr;
        const char* end;
        string value, version, buf;

        //If we are in preview mode, we only can retrieve atributes with mcuga and the response format is different
        if (isFromChatPreview())
        {
            ptr = client->json.getvalue();
            if (!ptr || !(end = strchr(ptr, '"')))
            {
                mCompletionErr(API_EINTERNAL);
            }
            else
            {
                // convert from ASCII to binary the received data
                buf.assign(ptr, (end-ptr));
                value.resize(buf.size() / 4 * 3 + 3);
                value.resize(Base64::atob(buf.data(), (byte *)value.data(), int(value.size())));
                mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);
            }
            return true;
        }

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case MAKENAMEID2('a','v'):
                {
                    if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
                    {
                        mCompletionErr(API_EINTERNAL);
                        if (client->fetchingkeys && at == ATTR_SIG_RSA_PUBK && u && u->userhandle == client->me)
                        {
                            client->initializekeys(); // we have now all the required data
                        }
                        return false;
                    }
                    buf.assign(ptr, (end-ptr));
                    break;
                }
                case 'v':
                {
                    if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
                    {
                        mCompletionErr(API_EINTERNAL);
                        if (client->fetchingkeys && at == ATTR_SIG_RSA_PUBK && u && u->userhandle == client->me)
                        {
                            client->initializekeys(); // we have now all the required data
                        }
                        return false;
                    }
                    version.assign(ptr, (end-ptr));
                    break;
                }
                case EOO:
                {
                    // if there's no avatar, the value is "none" (not Base64 encoded)
                    if (u && at == ATTR_AVATAR && buf == "none")
                    {
                        u->setattr(at, NULL, &version);
                        u->setTag(tag ? tag : -1);
                        mCompletionErr(API_ENOENT);
                        client->notifyuser(u);
                        return true;
                    }

                    // convert from ASCII to binary the received data
                    value.resize(buf.size() / 4 * 3 + 3);
                    value.resize(Base64::atob(buf.data(), (byte *)value.data(), int(value.size())));

                    // Some attributes don't keep historic records, ie. *!authring or *!lstint
                    // (none of those attributes are used by the SDK yet)
                    // bool nonHistoric = (attributename.at(1) == '!');

                    // handle the attribute data depending on the scope
                    char scope = User::scope(at);

                    if (!u) // retrieval of attributes without contact-relationship
                    {
                        if (at == ATTR_AVATAR && buf == "none")
                        {
                            mCompletionErr(API_ENOENT);
                        }
                        else
                        {
                            mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);
                        }
                        return true;
                    }

                    switch (scope)
                    {
                        case '*':   // private, encrypted
                        {
                            // decrypt the data and build the TLV records
                            std::unique_ptr<TLVstore> tlvRecords { TLVstore::containerToTLVrecords(&value, &client->key) };
                            if (!tlvRecords)
                            {
                                LOG_err << "Cannot extract TLV records for private attribute " << User::attr2string(at);
                                mCompletionErr(API_EINTERNAL);
                                return false;
                            }

                            // store the value for private user attributes (decrypted version of serialized TLV)
                            string *tlvString = tlvRecords->tlvRecordsToContainer(client->rng, &client->key);
                            u->setattr(at, tlvString, &version);
                            delete tlvString;
                            mCompletionTLV(tlvRecords.get(), at);

                            if (User::isAuthring(at))
                            {
                                client->mAuthRings.erase(at);
                                client->mAuthRings.emplace(at, AuthRing(at, *tlvRecords.get()));

                                if (client->mFetchingAuthrings && client->mAuthRings.size() == 3)
                                {
                                    client->mFetchingAuthrings = false;
                                    client->fetchContactsKeys();
                                }
                            }
                            break;
                        }
                        case '+':   // public
                        {
                            u->setattr(at, &value, &version);
                            mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);

                            if (client->fetchingkeys && at == ATTR_SIG_RSA_PUBK && u && u->userhandle == client->me)
                            {
                                client->initializekeys(); // we have now all the required data
                            }

                            if (!u->isTemporary && u->userhandle != client->me)
                            {
                                if (at == ATTR_ED25519_PUBK || at == ATTR_CU25519_PUBK)
                                {
                                    client->trackKey(at, u->userhandle, value);
                                }
                                else if (at == ATTR_SIG_CU255_PUBK || at == ATTR_SIG_RSA_PUBK)
                                {
                                    client->trackSignature(at, u->userhandle, value);
                                }
                            }
                            break;
                        }
                        case '#':   // protected
                        {
                            u->setattr(at, &value, &version);
                            mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);
                            break;
                        }
                        case '^': // private, non-encrypted
                        {
                            // store the value in cache in binary format
                            u->setattr(at, &value, &version);
                            mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);

                            if (at == ATTR_DISABLE_VERSIONS)
                            {
                                client->versions_disabled = !strcmp(value.data(), "1");
                                if (client->versions_disabled)
                                {
                                    LOG_info << "File versioning is disabled";
                                }
                                else
                                {
                                    LOG_info << "File versioning is enabled";
                                }
                            }
                            else if (at == ATTR_NO_CALLKIT)
                            {
                                LOG_info << "CallKit is " << ((!strcmp(value.data(), "1")) ? "disabled" : "enabled");
                            }
                            break;
                        }
                        default:    // legacy attributes or unknown attribute
                        {
                            if (at != ATTR_FIRSTNAME &&           // protected
                                    at != ATTR_LASTNAME &&        // protected
                                    at != ATTR_COUNTRY  &&        // private
                                    at != ATTR_BIRTHDAY &&        // private
                                    at != ATTR_BIRTHMONTH &&      // private
                                    at != ATTR_BIRTHYEAR)     // private
                            {
                                LOG_err << "Unknown received attribute: " << User::attr2string(at);
                                mCompletionErr(API_EINTERNAL);
                                return false;
                            }

                            u->setattr(at, &value, &version);
                            mCompletionBytes((byte*) value.data(), unsigned(value.size()), at);
                            break;
                        }

                    }   // switch (scope)

                    u->setTag(tag ? tag : -1);
                    client->notifyuser(u);
                    return true;
                }
                default:
                {
                    if (!client->json.storeobject())
                    {
                        LOG_err << "Error in CommandGetUA. Parse error";
                        client->app->getua_result(API_EINTERNAL);
                        if (client->fetchingkeys && at == ATTR_SIG_RSA_PUBK && u && u->userhandle == client->me)
                        {
                            client->initializekeys(); // we have now all the required data
                        }
                        return false;
                    }
                }

            }   // switch (nameid)
        }
    }
#ifndef WIN32
    return false;  // unreachable code
#endif
}

#ifdef DEBUG
CommandDelUA::CommandDelUA(MegaClient *client, const char *an)
{
    this->an = an;

    cmd("upr");
    arg("ua", an);

    arg("v", 1);    // returns the new version for the (removed) null value

    tag = client->reqtag;
}

bool CommandDelUA::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->delua_result(r.errorOrOK());
    }
    else
    {
        const char* ptr;
        const char* end;
        if (!(ptr = client->json.getvalue()) || !(end = strchr(ptr, '"')))
        {
            client->app->delua_result(API_EINTERNAL);
            return false;
        }

        User *u = client->ownuser();
        attr_t at = User::string2attr(an.c_str());
        string version(ptr, (end-ptr));

        u->removeattr(at, &version); // store version to filter corresponding AP in order to avoid double onUsersUpdate()

        if (at == ATTR_KEYRING)
        {
            client->resetKeyring();
        }
        else if (User::isAuthring(at))
        {
            client->mAuthRings.emplace(at, AuthRing(at, TLVstore()));
            client->getua(u, at, 0);
        }

        client->notifyuser(u);
        client->app->delua_result(API_OK);
    }
    return true;
}

CommandSendDevCommand::CommandSendDevCommand(MegaClient *client, const char *command, const char *email, long long q, int bs, int us)
{
    cmd("dev");

    arg("aa", command);
    if (email)
    {
        arg("t", email);
    }

    if ((strcmp(command, "tq") == 0))
    {
        arg("q", q);
    }
    else if ((strcmp(command, "bs") == 0))
    {
        arg("s", bs);
    }
    else if ((strcmp(command, "us") == 0))
    {
        arg("s", us);
    }
    tag = client->reqtag;
}

bool CommandSendDevCommand::procresult(Result r)
{
    client->app->senddevcommand_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

#endif  // #ifdef DEBUG

CommandGetUserEmail::CommandGetUserEmail(MegaClient *client, const char *uid)
{
    cmd("uge");
    arg("u", uid);

    tag = client->reqtag;
}

bool CommandGetUserEmail::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getuseremail_result(NULL, r.errorOrOK());
        return true;
    }

    string email;
    if (!client->json.storeobject(&email))
    {
        client->app->getuseremail_result(NULL, API_EINTERNAL);
        return false;
    }
    else
    {
        client->app->getuseremail_result(&email, API_OK);
        return true;
    }
}

// set node keys (e.g. to convert asymmetric keys to symmetric ones)
CommandNodeKeyUpdate::CommandNodeKeyUpdate(MegaClient* client, handle_vector* v)
{
    byte nodekey[FILENODEKEYLENGTH];

    cmd("k");
    beginarray("nk");

    for (size_t i = v->size(); i--;)
    {
        handle h = (*v)[i];

        Node* n;

        if ((n = client->nodebyhandle(h)))
        {
            client->key.ecb_encrypt((byte*)n->nodekey().data(), nodekey, n->nodekey().size());

            element(h, MegaClient::NODEHANDLE);
            element(nodekey, int(n->nodekey().size()));
        }
    }

    endarray();
}

CommandSingleKeyCR::CommandSingleKeyCR(handle sh, handle nh, const byte* key, size_t keylen)
{
    cmd("k");
    beginarray("cr");

    beginarray();
    element(sh, MegaClient::NODEHANDLE);
    endarray();

    beginarray();
    element(nh, MegaClient::NODEHANDLE);
    endarray();

    beginarray();
    element(0);
    element(0);
    element(key, static_cast<int>(keylen));
    endarray();

    endarray();
}

CommandKeyCR::CommandKeyCR(MegaClient* /*client*/, node_vector* rshares, node_vector* rnodes, const char* keys)
{
    cmd("k");
    beginarray("cr");

    beginarray();
    for (int i = 0; i < (int)rshares->size(); i++)
    {
        element((*rshares)[i]->nodehandle, MegaClient::NODEHANDLE);
    }

    endarray();

    beginarray();
    for (int i = 0; i < (int)rnodes->size(); i++)
    {
        element((*rnodes)[i]->nodehandle, MegaClient::NODEHANDLE);
    }

    endarray();

    beginarray();
    appendraw(keys);
    endarray();

    endarray();
}

// a == ACCESS_UNKNOWN: request public key for user handle and respond with
// share key for sn
// otherwise: request public key for user handle and continue share creation
// for node sn to user u with access a
CommandPubKeyRequest::CommandPubKeyRequest(MegaClient* client, User* user)
{
    cmd("uk");
    arg("u", user->uid.c_str());

    u = user;
    tag = client->reqtag;
}

bool CommandPubKeyRequest::procresult(Result r)
{
    byte pubkbuf[AsymmCipher::MAXKEYLENGTH];
    int len_pubk = 0;
    handle uh = UNDEF;

    if (r.wasErrorOrOK())
    {
        if (!r.wasError(API_ENOENT)) //API_ENOENT = unregistered users or accounts without a public key yet
        {
            LOG_err << "Unexpected error in CommandPubKeyRequest: " << error(r.errorOrOK());
        }
    }
    else
    {
        bool finished = false;
        while (!finished)
        {
            switch (client->json.getnameid())
            {
                case 'u':
                    uh = client->json.gethandle(MegaClient::USERHANDLE);
                    break;

                case MAKENAMEID4('p', 'u', 'b', 'k'):
                    len_pubk = client->json.storebinary(pubkbuf, sizeof pubkbuf);
                    break;

                case EOO:
                    if (!u) // user has cancelled the account
                    {
                        return true;
                    }

                    if (!ISUNDEF(uh))
                    {
                        client->mapuser(uh, u->email.c_str());
                        if (u->isTemporary && u->uid == u->email) //update uid with the received USERHANDLE (will be used as target for putnodes)
                        {
                            u->uid = Base64Str<MegaClient::USERHANDLE>(uh);
                        }
                    }

                    if (client->fetchingkeys && u->userhandle == client->me && len_pubk)
                    {
                        client->pubk.setkey(AsymmCipher::PUBKEY, pubkbuf, len_pubk);
                        return true;
                    }

                    if (len_pubk && !u->pubk.setkey(AsymmCipher::PUBKEY, pubkbuf, len_pubk))
                    {
                        len_pubk = 0;
                    }

                    if (!u->isTemporary && u->userhandle != client->me && len_pubk && u->pubk.isvalid())
                    {
                        string pubkstr;
                        u->pubk.serializekeyforjs(pubkstr);
                        client->trackKey(ATTR_UNKNOWN, u->userhandle, pubkstr);
                    }
                    finished = true;
                    break;

                default:
                    if (client->json.storeobject())
                    {
                        continue;
                    }
                    len_pubk = 0;
                    finished = true;
                    break;
            }
        }
    }

    if (!u) // user has cancelled the account, or HIDDEN user was removed
    {
        return true;
    }

    // satisfy all pending PubKeyAction requests for this user
    while (u->pkrs.size())
    {
        client->restag = tag;
        u->pkrs[0]->proc(client, u);
        u->pkrs.pop_front();
    }

    if (len_pubk && !u->isTemporary)
    {
        client->notifyuser(u);
    }

    if (u->isTemporary)
    {
        delete u;
        u = NULL;
    }

    return true;
}

void CommandPubKeyRequest::invalidateUser()
{
    u = NULL;
}

CommandGetUserData::CommandGetUserData(MegaClient *client, int tag, std::function<void(string*, string*, string*, error)> completion)
{
    cmd("ug");
    arg("v", 1);

    this->tag = tag;

    mCompletion = completion ? move(completion) :
        [this](string* name, string* pubk, string* privk, error e) {
            this->client->app->userdata_result(name, pubk, privk, e);
        };

}

bool CommandGetUserData::procresult(Result r)
{
    string name;
    string pubk;
    string privk;
    string k;
    byte privkbuf[AsymmCipher::MAXKEYLENGTH * 2];
    int len_privk = 0;
    byte pubkbuf[AsymmCipher::MAXKEYLENGTH];
    int len_pubk = 0;
    m_time_t since = 0;
    int v = 0;
    string salt;
    string smsv;
    string lastname;
    string versionLastname;
    string firstname;
    string versionFirstname;
    string language;
    string versionLanguage;
    string pwdReminderDialog;
    string versionPwdReminderDialog;
    string pushSetting;
    string versionPushSetting;
    string contactLinkVerification;
    string versionContactLinkVerification;
    handle me = UNDEF;
    string chatFolder;
    string versionChatFolder;
    string cameraUploadFolder;
    string versionCameraUploadFolder;
    string aliases;
    string versionAliases;
    string disableVersions;
    string versionDisableVersions;
    string noCallKit;
    string versionNoCallKit;
    string country;
    string versionCountry;
    string birthday;
    string versionBirthday;
    string birthmonth;
    string versionBirthmonth;
    string birthyear;
    string versionBirthyear;
    string email;
    string unshareableKey;
    string versionUnshareableKey;
    string deviceNames;
    string versionDeviceNames;
    string driveNames;
    string versionDriveNames;
    string myBackupsFolder;
    string versionMyBackupsFolder;
    string versionBackupNames;
    string cookieSettings;
    string versionCookieSettings;
#ifdef ENABLE_SYNC
    string jsonSyncConfigData;
    string jsonSyncConfigDataVersion;
#endif

    bool uspw = false;
    vector<m_time_t> warningTs;
    m_time_t deadlineTs = -1;

    bool b = false;
    BizMode m = BIZ_MODE_UNKNOWN;
    BizStatus s = BIZ_STATUS_UNKNOWN;
    std::set<handle> masters;
    std::vector<std::pair<BizStatus, m_time_t>> sts;

    if (r.wasErrorOrOK())
    {
        mCompletion(NULL, NULL, NULL, r.wasError(API_OK) ? Error(API_ENOENT) : r.errorOrOK());
        return true;
    }

    for (;;)
    {
        string attributeName = client->json.getnameWithoutAdvance();
        switch (client->json.getnameid())
        {
        case MAKENAMEID3('a', 'a', 'v'):    // account authentication version
            v = (int)client->json.getint();
            break;

        case MAKENAMEID3('a', 'a', 's'):    // account authentication salt
            client->json.storeobject(&salt);
            break;

        case MAKENAMEID4('n', 'a', 'm', 'e'):
            client->json.storeobject(&name);
            break;

        case 'k':   // master key
            k.resize(SymmCipher::KEYLENGTH);
            client->json.storebinary((byte *)k.data(), int(k.size()));
            break;

        case MAKENAMEID5('s', 'i', 'n', 'c', 'e'):
            since = client->json.getint();
            break;

        case MAKENAMEID4('p', 'u', 'b', 'k'):   // RSA public key
            client->json.storeobject(&pubk);
            len_pubk = Base64::atob(pubk.c_str(), pubkbuf, sizeof pubkbuf);
            break;

        case MAKENAMEID5('p', 'r', 'i', 'v', 'k'):  // RSA private key (encrypted to MK)
            len_privk = client->json.storebinary(privkbuf, sizeof privkbuf);
            break;

        case MAKENAMEID5('f', 'l', 'a', 'g', 's'):
            if (client->json.enterobject())
            {
                if (client->readmiscflags(&client->json) != API_OK)
                {
                    mCompletion(NULL, NULL, NULL, API_EINTERNAL);
                    return false;
                }
                client->json.leaveobject();
            }
            break;

        case 'u':
            me = client->json.gethandle(MegaClient::USERHANDLE);
            break;

        case MAKENAMEID8('l', 'a', 's', 't', 'n', 'a', 'm', 'e'):
            parseUserAttribute(lastname, versionLastname);
            break;

        case MAKENAMEID6('^', '!', 'l', 'a', 'n', 'g'):
            parseUserAttribute(language, versionLanguage);
            break;

        case MAKENAMEID8('b', 'i', 'r', 't', 'h', 'd', 'a', 'y'):
            parseUserAttribute(birthday, versionBirthday);
            break;

        case MAKENAMEID7('c', 'o', 'u', 'n', 't', 'r', 'y'):
            parseUserAttribute(country, versionCountry);
            break;

        case MAKENAMEID4('^', '!', 'p', 's'):
            parseUserAttribute(pushSetting, versionPushSetting);
            break;

        case MAKENAMEID5('^', '!', 'p', 'r', 'd'):
            parseUserAttribute(pwdReminderDialog, versionPwdReminderDialog);
            break;

        case MAKENAMEID4('^', 'c', 'l', 'v'):
            parseUserAttribute(contactLinkVerification, versionContactLinkVerification);
            break;

        case MAKENAMEID4('^', '!', 'd', 'v'):
            parseUserAttribute(disableVersions, versionDisableVersions);
            break;

        case MAKENAMEID7('^', '!', 'n', 'o', 'k', 'i', 't'):
            parseUserAttribute(noCallKit, versionNoCallKit);
            break;

        case MAKENAMEID4('*', '!', 'c', 'f'):
            parseUserAttribute(chatFolder, versionChatFolder);
            break;

        case MAKENAMEID5('*', '!', 'c', 'a', 'm'):
            parseUserAttribute(cameraUploadFolder, versionCameraUploadFolder);
            break;

        case MAKENAMEID8('*', '!', '>', 'a', 'l', 'i', 'a', 's'):
            parseUserAttribute(aliases, versionAliases);
            break;

        case MAKENAMEID5('e', 'm', 'a', 'i', 'l'):
            client->json.storeobject(&email);
            break;

        case MAKENAMEID5('*', '~', 'u', 's', 'k'):
            parseUserAttribute(unshareableKey, versionUnshareableKey, false);
            break;

        case MAKENAMEID4('*', '!', 'd', 'n'):
            parseUserAttribute(deviceNames, versionDeviceNames);
            break;

        case MAKENAMEID5('*', '!', 'd', 'r', 'n'):
            parseUserAttribute(driveNames, versionDriveNames);
            break;

        case MAKENAMEID5('^', '!', 'b', 'a', 'k'):
            parseUserAttribute(myBackupsFolder, versionMyBackupsFolder);
            break;

#ifdef ENABLE_SYNC
        case MAKENAMEID6('*', '~', 'j', 's', 'c', 'd'):
            parseUserAttribute(jsonSyncConfigData, jsonSyncConfigDataVersion);
            break;
#endif

        case MAKENAMEID2('p', 'f'):  // Pro Flexi plan (similar to business)
            [[fallthrough]];
        case 'b':   // business account's info
            assert(!b);
            b = true;
            if (client->json.enterobject())
            {
                bool endobject = false;
                while (!endobject)
                {
                    switch (client->json.getnameid())
                    {
                        case 's':   // status
                            // -1: expired, 1: active, 2: grace-period
                            s = BizStatus(client->json.getint32());
                            break;

                        case 'm':   // mode
                            m = BizMode(client->json.getint32());
                            break;

                        case MAKENAMEID2('m', 'u'):
                            if (client->json.enterarray())
                            {
                                for (;;)
                                {
                                    handle uh = client->json.gethandle(MegaClient::USERHANDLE);
                                    if (!ISUNDEF(uh))
                                    {
                                        masters.emplace(uh);
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                                client->json.leavearray();
                            }
                            break;

                        case MAKENAMEID3('s', 't', 's'):    // status timestamps
                            // ie. "sts":[{"s":-1,"ts":1566182227},{"s":1,"ts":1563590227}]
                            client->json.enterarray();
                            while (client->json.enterobject())
                            {
                                BizStatus status = BIZ_STATUS_UNKNOWN;
                                m_time_t ts = 0;

                                bool exit = false;
                                while (!exit)
                                {
                                    switch (client->json.getnameid())
                                    {
                                        case 's':
                                           status = BizStatus(client->json.getint());
                                           break;

                                        case MAKENAMEID2('t', 's'):
                                           ts = client->json.getint();
                                           break;

                                        case EOO:
                                            if (status != BIZ_STATUS_UNKNOWN && ts != 0)
                                            {
                                                sts.push_back(std::make_pair(status, ts));
                                            }
                                            else
                                            {
                                                LOG_warn << "Unpaired/missing business status-ts in b.sts";
                                            }
                                            exit = true;
                                            break;

                                        default:
                                            if (!client->json.storeobject())
                                            {
                                                mCompletion(NULL, NULL, NULL, API_EINTERNAL);
                                                return false;
                                            }
                                    }
                                }
                                client->json.leaveobject();
                            }
                            client->json.leavearray();
                            break;

                        case EOO:
                            endobject = true;
                            break;

                        default:
                            if (!client->json.storeobject())
                            {
                                mCompletion(NULL, NULL, NULL, API_EINTERNAL);
                                return false;
                            }
                    }
                }
                client->json.leaveobject();
            }
            break;

        case MAKENAMEID4('s', 'm', 's', 'v'):   // SMS verified phone number
            if (!client->json.storeobject(&smsv))
            {
                LOG_err << "Invalid verified phone number (smsv)";
                assert(false);
            }
            break;

        case MAKENAMEID4('u', 's', 'p', 'w'):   // user paywall data
        {
            uspw = true;

            if (client->json.enterobject())
            {
                bool endobject = false;
                while (!endobject)
                {
                    switch (client->json.getnameid())
                    {
                        case MAKENAMEID2('d', 'l'): // deadline timestamp
                            deadlineTs = client->json.getint();
                            break;

                        case MAKENAMEID3('w', 't', 's'):    // warning timestamps
                            // ie. "wts":[1591803600,1591813600,1591823600

                            if (client->json.enterarray())
                            {
                                m_time_t ts;
                                while (client->json.isnumeric() && (ts = client->json.getint()) != -1)
                                {
                                    warningTs.push_back(ts);
                                }

                                client->json.leavearray();
                            }
                            break;

                        case EOO:
                            endobject = true;
                            break;

                        default:
                            if (!client->json.storeobject())
                            {
                                mCompletion(NULL, NULL, NULL, API_EINTERNAL);
                                return false;
                            }
                    }
                }
                client->json.leaveobject();
            }
            break;
        }

        case MAKENAMEID5('^', '!', 'c', 's', 'p'):
            parseUserAttribute(cookieSettings, versionCookieSettings);
            break;

//        case MAKENAMEID1('p'):  // plan: 101 for Pro Flexi
//            {
//                int proPlan = client->json.getint32();
//            }
//            break;
        case EOO:
        {
            assert(me == client->me);

            if (len_privk)
            {
                client->key.ecb_decrypt(privkbuf, len_privk);
                privk.resize(AsymmCipher::MAXKEYLENGTH * 2);
                privk.resize(Base64::btoa(privkbuf, len_privk, (char *)privk.data()));

                // RSA private key should be already assigned at login
                assert(privk == client->mPrivKey);
                if (client->mPrivKey.empty())
                {
                    LOG_warn << "Private key not set by login, setting at `ug` response...";
                    if (!client->asymkey.setkey(AsymmCipher::PRIVKEY, privkbuf, len_privk))
                    {
                        LOG_warn << "Error checking private key at `ug` response";
                    }
                }
            }

            if (len_pubk)
            {
                client->pubk.setkey(AsymmCipher::PUBKEY, pubkbuf, len_pubk);
            }

            if (v)
            {
                client->accountversion = v;
            }

            if (salt.size())
            {
                Base64::atob(salt, client->accountsalt);
            }

            client->accountsince = since;
            client->mSmsVerifiedPhone = smsv;

            client->k = k;

            client->btugexpiration.backoff(MegaClient::USER_DATA_EXPIRATION_BACKOFF_SECS * 10);
            client->cachedug = true;

            // pre-load received user attributes into cache
            User* u = client->ownuser();
            if (u)
            {
                int changes = 0;
                if (u->email.empty())
                {
                    u->email = email;
                }

                if (firstname.size())
                {
                    changes += u->updateattr(ATTR_FIRSTNAME, &firstname, &versionFirstname);
                }

                if (lastname.size())
                {
                    changes += u->updateattr(ATTR_LASTNAME, &lastname, &versionLastname);
                }

                if (language.size())
                {
                    changes += u->updateattr(ATTR_LANGUAGE, &language, &versionLanguage);
                }

                if (birthday.size())
                {
                    changes += u->updateattr(ATTR_BIRTHDAY, &birthday, &versionBirthday);
                }

                if (birthmonth.size())
                {
                    changes += u->updateattr(ATTR_BIRTHMONTH, &birthmonth, &versionBirthmonth);
                }

                if (birthyear.size())
                {
                    changes += u->updateattr(ATTR_BIRTHYEAR, &birthyear, &versionBirthyear);
                }

                if (country.size())
                {
                    changes += u->updateattr(ATTR_COUNTRY, &country, &versionCountry);
                }

                if (pwdReminderDialog.size())
                {
                    changes += u->updateattr(ATTR_PWD_REMINDER, &pwdReminderDialog, &versionPwdReminderDialog);
                }

                if (pushSetting.size())
                {
                    changes += u->updateattr(ATTR_PUSH_SETTINGS, &pushSetting, &versionPushSetting);

                    // initialize the settings for the intermediate layer by simulating there was a getua()
                    client->app->getua_result((byte*) pushSetting.data(), (unsigned) pushSetting.size(), ATTR_PUSH_SETTINGS);
                }

                if (contactLinkVerification.size())
                {
                    changes += u->updateattr(ATTR_CONTACT_LINK_VERIFICATION, &contactLinkVerification, &versionContactLinkVerification);
                }

                if (disableVersions.size())
                {
                    changes += u->updateattr(ATTR_DISABLE_VERSIONS, &disableVersions, &versionDisableVersions);

                    // initialize the status of file-versioning for the client
                    client->versions_disabled = (disableVersions == "1");
                    if (client->versions_disabled)
                    {
                        LOG_info << "File versioning is disabled";
                    }
                    else
                    {
                        LOG_info << "File versioning is enabled";
                    }
                }
                else    // attribute does not exists
                {
                    LOG_info << "File versioning is enabled";
                    client->versions_disabled = false;
                }

                if (noCallKit.size())
                {
                    changes += u->updateattr(ATTR_NO_CALLKIT, &noCallKit, &versionNoCallKit);
                    LOG_info << "CallKit is " << ((noCallKit == "1") ? "disabled" : "enabled");
                }
                else
                {
                    LOG_info << "CallKit is enabled [noCallKit.size() == 0]";
                }

                if (chatFolder.size())
                {
                    unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&chatFolder, &client->key));
                    if (tlvRecords)
                    {
                        // store the value for private user attributes (decrypted version of serialized TLV)
                        unique_ptr<string> tlvString(tlvRecords->tlvRecordsToContainer(client->rng, &client->key));
                        changes += u->updateattr(ATTR_MY_CHAT_FILES_FOLDER, tlvString.get(), &versionChatFolder);
                    }
                    else
                    {
                        LOG_err << "Cannot extract TLV records for ATTR_MY_CHAT_FILES_FOLDER";
                    }
                }

                if (cameraUploadFolder.size())
                {
                    unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&cameraUploadFolder, &client->key));
                    if (tlvRecords)
                    {
                        // store the value for private user attributes (decrypted version of serialized TLV)
                        unique_ptr<string> tlvString(tlvRecords->tlvRecordsToContainer(client->rng, &client->key));
                        changes += u->updateattr(ATTR_CAMERA_UPLOADS_FOLDER, tlvString.get(), &versionCameraUploadFolder);
                    }
                    else
                    {
                        LOG_err << "Cannot extract TLV records for ATTR_CAMERA_UPLOADS_FOLDER";
                    }
                }

                if (!myBackupsFolder.empty())
                {
                    changes += u->updateattr(ATTR_MY_BACKUPS_FOLDER, &myBackupsFolder, &versionMyBackupsFolder);
                }

                if (aliases.size())
                {
                    unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&aliases, &client->key));
                    if (tlvRecords)
                    {
                        // store the value for private user attributes (decrypted version of serialized TLV)
                        unique_ptr<string> tlvString(tlvRecords->tlvRecordsToContainer(client->rng, &client->key));
                        changes += u->updateattr(ATTR_ALIAS, tlvString.get(), &versionAliases);
                    }
                    else
                    {
                        LOG_err << "Cannot extract TLV records for ATTR_ALIAS";
                    }
                }

                if (unshareableKey.size() == Base64Str<SymmCipher::BLOCKSIZE>::STRLEN)
                {
                    changes += u->updateattr(ATTR_UNSHAREABLE_KEY, &unshareableKey, &versionUnshareableKey);
                    client->unshareablekey.swap(unshareableKey);
                }
                else if (client->loggedin() == EPHEMERALACCOUNTPLUSPLUS)
                {
                    // cannot configure CameraUploads, so it's not needed at this stage.
                    // It will be created when the account gets confirmed.
                    // (motivation: speed up the E++ account's setup)
                    LOG_info << "Skip creation of unshareable key for E++ account";
                }
                else if (unshareableKey.empty())    // it has not been created yet
                {
                    LOG_info << "Creating unshareable key...";
                    byte newunshareablekey[SymmCipher::BLOCKSIZE];
                    client->rng.genblock(newunshareablekey, sizeof(newunshareablekey));
                    client->putua(ATTR_UNSHAREABLE_KEY, newunshareablekey, sizeof(newunshareablekey), 0);
                }
                else
                {
                    LOG_err << "Unshareable key wrong length";
                }

                if (deviceNames.size())
                {
                    unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&deviceNames, &client->key));
                    if (tlvRecords)
                    {
                        // store the value for private user attributes (decrypted version of serialized TLV)
                        unique_ptr<string> tlvString(tlvRecords->tlvRecordsToContainer(client->rng, &client->key));
                        changes += u->updateattr(ATTR_DEVICE_NAMES, tlvString.get(), &versionDeviceNames);
                    }
                    else
                    {
                        LOG_err << "Cannot extract TLV records for ATTR_DEVICE_NAMES";
                    }
                }

                if (!driveNames.empty())
                {
                    unique_ptr<TLVstore> tlvRecords(TLVstore::containerToTLVrecords(&driveNames, &client->key));
                    if (tlvRecords)
                    {
                        // store the value for private user attributes (decrypted version of serialized TLV)
                        unique_ptr<string> tlvString(tlvRecords->tlvRecordsToContainer(client->rng, &client->key));
                        changes += u->updateattr(ATTR_DRIVE_NAMES, tlvString.get(), &versionDriveNames);
                    }
                    else
                    {
                        LOG_err << "Cannot extract TLV records for ATTR_DRIVE_NAMES";
                    }
                }

                if (!cookieSettings.empty())
                {
                    changes += u->updateattr(ATTR_COOKIE_SETTINGS, &cookieSettings, &versionCookieSettings);
                }

#ifdef ENABLE_SYNC
                if (!jsonSyncConfigData.empty())
                {
                    // Tell the rest of the SDK that the attribute's changed.
                    changes += u->updateattr(ATTR_JSON_SYNC_CONFIG_DATA,
                                             &jsonSyncConfigData,
                                             &jsonSyncConfigDataVersion);
                }
                else if (client->loggedin() == EPHEMERALACCOUNTPLUSPLUS)
                {
                    // cannot configure any sync/backupp yet, so it's not needed at this stage.
                    // It will be created when the account gets confirmed.
                    // (motivation: speed up the E++ account's setup)
                    LOG_info << "Skip creation of *~jscd key for E++ account";
                }
                else
                {
                    // This attribute is set only once. If not received from API,
                    // it should not exist locally either
                    assert(u->getattr(ATTR_JSON_SYNC_CONFIG_DATA) == nullptr);

                    client->ensureSyncUserAttributes([](Error e){
                        if (e != API_OK)
                        {
                            LOG_err << "Couldn't create *~jscd user's attribute";
                        }
                    });
                }
#endif

                if (changes > 0)
                {
                    u->setTag(tag ? tag : -1);
                    client->notifyuser(u);
                }
            }

            if (b)  // business account
            {
                // integrity checks
                if ((s < BIZ_STATUS_EXPIRED || s > BIZ_STATUS_GRACE_PERIOD)  // status not received or invalid
                        || (m == BIZ_MODE_UNKNOWN && !client->isProFlexi()))  // master flag not received or invalid (or Pro Flexi, not business)
                {
                    std::string err = "GetUserData: invalid business status / account mode";
                    LOG_err << err;
                    client->sendevent(99450, err.c_str(), 0);
                    client->mBizMode = BIZ_MODE_SUBUSER;
                    client->mBizExpirationTs = client->mBizGracePeriodTs = 0;
                    client->setBusinessStatus(BIZ_STATUS_EXPIRED);
                }
                else
                {
                    for (auto it : sts)
                    {
                        BizStatus status = it.first;
                        m_time_t ts = it.second;
                        if (status == BIZ_STATUS_EXPIRED)
                        {
                            client->mBizExpirationTs = ts;
                        }
                        else if (status == BIZ_STATUS_GRACE_PERIOD)
                        {
                            client->mBizGracePeriodTs = ts;
                        }
                        else
                        {
                            LOG_warn << "Unexpected status in b.sts. Status: " << status << "ts: " << ts;
                        }
                    }

                    client->mBizMode = m;
                    // subusers must receive the list of master users
                    assert(m != BIZ_MODE_SUBUSER || !masters.empty());
                    client->mBizMasters = masters;

                    client->setBusinessStatus(s);

                    // if current business status will expire sooner than the scheduled `ug`, update the
                    // backoff to a shorter one in order to refresh the business status asap
                    m_time_t auxts = 0;
                    m_time_t now = m_time(nullptr);
                    if (client->mBizGracePeriodTs && client->mBizGracePeriodTs > now)
                    {
                        auxts = client->mBizGracePeriodTs;
                    }
                    else if (client->mBizExpirationTs && client->mBizExpirationTs > now)
                    {
                        auxts = client->mBizExpirationTs;
                    }
                    if (auxts)
                    {
                        dstime diff = static_cast<dstime>((now - auxts) * 10);
                        dstime current = client->btugexpiration.backoffdelta();
                        if (current > diff)
                        {
                            client->btugexpiration.backoff(diff);
                        }
                    }
                    // TODO: check if type of account has changed and notify with new event (not yet supported by API)
                }
            }
            else
            {
                client->mBizMode = BIZ_MODE_UNKNOWN;
                client->mBizMasters.clear();
                client->mBizExpirationTs = client->mBizGracePeriodTs = 0;
                client->setBusinessStatus(BIZ_STATUS_INACTIVE);
            }

            if (uspw)
            {
                if (deadlineTs == -1 || warningTs.empty())
                {
                    LOG_err << "uspw received with missing timestamps";
                }
                else
                {
                    client->mOverquotaWarningTs = std::move(warningTs);
                    client->mOverquotaDeadlineTs = deadlineTs;
                    client->activateoverquota(0, true);
                }

            }

            mCompletion(&name, &pubk, &privk, API_OK);
            return true;
        }
        default:
            switch (User::string2attr(attributeName.c_str()))
            {
                case ATTR_FIRSTNAME:
                    parseUserAttribute(firstname, versionFirstname);
                    break;

                case ATTR_BIRTHMONTH:
                    parseUserAttribute(birthmonth, versionBirthmonth);
                    break;

                case ATTR_BIRTHYEAR:
                    parseUserAttribute(birthyear, versionBirthyear);
                    break;

                default:
                    if (!client->json.storeobject())
                    {
                        mCompletion(NULL, NULL, NULL, API_EINTERNAL);
                        return false;
                    }
                    break;
            }

            break;
        }
    }
}

void CommandGetUserData::parseUserAttribute(std::string &value, std::string &version, bool asciiToBinary)
{
    string info;
    if (!client->json.storeobject(&info))
    {
        LOG_err << "Failed to parse user attribute from the array";
        return;
    }

    string buf;
    JSON json;
    json.pos = info.c_str() + 1;
    for (;;)
    {
        switch (json.getnameid())
        {
            case MAKENAMEID2('a','v'):  // value
            {
                json.storeobject(&buf);
                break;
            }
            case 'v':   // version
            {
                json.storeobject(&version);
                break;
            }
            case EOO:
            {
                value = asciiToBinary ? Base64::atob(buf) : buf;
                return;
            }
            default:
            {
                if (!json.storeobject())
                {
                    version.clear();
                    LOG_err << "Failed to parse user attribute inside the array";
                    return;
                }
            }
        }
    }
}

CommandGetMiscFlags::CommandGetMiscFlags(MegaClient *client)
{
    cmd("gmf");

    // this one can get the smsve flag when the account is blocked (if it's in a batch by itself)
    batchSeparately = true;
    suppressSID = true;

    tag = client->reqtag;
}

bool CommandGetMiscFlags::procresult(Result r)
{
    Error e;
    if (r.wasErrorOrOK())
    {
        e = r.errorOrOK();
        if (!e)
        {
            LOG_err << "Unexpected response for gmf: no flags, but no error";
            e = API_ENOENT;
        }
        LOG_err << "gmf failed: " << e;
    }
    else
    {
        e = client->readmiscflags(&client->json);
    }

    client->app->getmiscflags_result(e);
    return error(e) != API_EINTERNAL;
}

CommandGetUserQuota::CommandGetUserQuota(MegaClient* client, std::shared_ptr<AccountDetails> ad, bool storage, bool transfer, bool pro, int source)
{
    details = ad;
    mStorage = storage;
    mTransfer = transfer;
    mPro = pro;

    cmd("uq");
    if (storage)
    {
        arg("strg", "1", 0);
    }
    if (transfer)
    {
        arg("xfer", "1", 0);
    }
    if (pro)
    {
        arg("pro", "1", 0);
    }

    arg("src", source);

    arg("v", 1);

    tag = client->reqtag;
}

bool CommandGetUserQuota::procresult(Result r)
{
    m_off_t td;
    bool got_storage = false;
    bool got_storage_used = false;
    int uslw = -1;

    if (r.wasErrorOrOK())
    {
        client->app->account_details(details.get(), r.errorOrOK());
        return true;
    }

    details->pro_level = 0;
    details->subscription_type = 'O';
    details->subscription_renew = 0;
    details->subscription_method.clear();
    details->subscription_method_id = 0;
    memset(details->subscription_cycle, 0, sizeof(details->subscription_cycle));

    details->pro_until = 0;

    details->storage_used = 0;
    details->storage_max = 0;

    details->transfer_max = 0;
    details->transfer_own_used = 0;
    details->transfer_srv_used = 0;
    details->srv_ratio = 0;

    details->transfer_hist_starttime = 0;
    details->transfer_hist_interval = 3600;
    details->transfer_hist.clear();
    details->transfer_hist_valid = true;

    details->transfer_reserved = 0;
    details->transfer_own_reserved = 0;
    details->transfer_srv_reserved = 0;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID2('b', 't'):
            // "Base time age", this is number of seconds since the start of the current quota buckets
                // age of transfer
                // window start
                td = client->json.getint();
                if (td != -1)
                {
                    details->transfer_hist_starttime = m_time() - td;
                }
                break;

            case MAKENAMEID3('t', 'a', 'h'):
            // The free IP-based quota buckets, 6 entries for 6 hours
                if (client->json.enterarray())
                {
                    m_off_t t;

                    while (client->json.isnumeric() && (t = client->json.getint()) != -1)
                    {
                        details->transfer_hist.push_back(t);
                    }

                    client->json.leavearray();
                }
                break;

            case MAKENAMEID3('t', 'a', 'r'):
            // IP transfer reserved
                details->transfer_reserved = client->json.getint();
                break;

            case MAKENAMEID3('r', 'u', 'a'):
            // Actor reserved quota
                details->transfer_own_reserved += client->json.getint();
                break;

            case MAKENAMEID3('r', 'u', 'o'):
            // Owner reserved quota
                details->transfer_srv_reserved += client->json.getint();
                break;

            case MAKENAMEID5('c', 's', 't', 'r', 'g'):
            // Your total account storage usage
                details->storage_used = client->json.getint();
                got_storage_used = true;
                break;

            case MAKENAMEID6('c', 's', 't', 'r', 'g', 'n'):
            // Storage breakdown of root nodes and shares for your account
            // [bytes, numFiles, numFolders, versionedBytes, numVersionedFiles]
                if (client->json.enterobject())
                {
                    handle h;
                    NodeStorage* ns;

                    while (!ISUNDEF(h = client->json.gethandle()) && client->json.enterarray())
                    {
                        ns = &details->storage[h];

                        ns->bytes = client->json.getint();
                        ns->files = uint32_t(client->json.getint());
                        ns->folders = uint32_t(client->json.getint());
                        ns->version_bytes = client->json.getint();
                        ns->version_files = client->json.getint32();

#ifdef _DEBUG
                        // TODO: remove this debugging block once local count is confirmed to work correctly 100%
                        // verify the new local storage counters per root match server side (could fail if actionpackets are pending)
                        auto iter = client->mNodeCounters.find(NodeHandle().set6byte(h));
                        if (iter != client->mNodeCounters.end())
                        {
                            LOG_debug << client->nodebyhandle(h)->displaypath() << " " << iter->second.storage << " " << ns->bytes << " " << iter->second.files << " " << ns->files << " " << iter->second.folders << " " << ns->folders << " "
                                      << iter->second.versionStorage << " " << ns->version_bytes << " " << iter->second.versions << " " << ns->version_files
                                      << (iter->second.storage == ns->bytes && iter->second.files == ns->files && iter->second.folders == ns->folders && iter->second.versionStorage == ns->version_bytes && iter->second.versions == ns->version_files
                                          ? "" : " ******************************************* mismatch *******************************************");
                        }
#endif

                        while(client->json.storeobject());
                        client->json.leavearray();
                    }

                    client->json.leaveobject();
                }
                break;

            case MAKENAMEID5('m', 's', 't', 'r', 'g'):
            // maximum storage allowance
                details->storage_max = client->json.getint();
                got_storage = true;
                break;

            case MAKENAMEID6('c', 'a', 'x', 'f', 'e', 'r'):
            // PRO transfer quota consumed by yourself
                details->transfer_own_used += client->json.getint();
                break;

            case MAKENAMEID3('t', 'u', 'o'):
            // Transfer usage by the owner on quotad which hasn't yet been committed back to the API DB. Supplements caxfer
                details->transfer_own_used += client->json.getint();
                break;

            case MAKENAMEID6('c', 's', 'x', 'f', 'e', 'r'):
            // PRO transfer quota served to others
                details->transfer_srv_used += client->json.getint();
                break;

            case MAKENAMEID3('t', 'u', 'a'):
            // Transfer usage served to other users which hasn't yet been committed back to the API DB. Supplements csxfer
                details->transfer_srv_used += client->json.getint();
                break;

            case MAKENAMEID5('m', 'x', 'f', 'e', 'r'):
            // maximum transfer allowance
                details->transfer_max = client->json.getint();
                break;

            case MAKENAMEID8('s', 'r', 'v', 'r', 'a', 't', 'i', 'o'):
            // The ratio of your PRO transfer quota that is able to be served to others
                details->srv_ratio = client->json.getfloat();
                break;

            case MAKENAMEID5('u', 't', 'y', 'p', 'e'):
            // PRO type. 0 means Free; 4 is Pro Lite as it was added late; 100 indicates a business.
                details->pro_level = (int)client->json.getint();
                client->mMyAccount.setProLevel(static_cast<AccountType>(details->pro_level));
                break;

            case MAKENAMEID5('s', 't', 'y', 'p', 'e'):
            // Flag indicating if this is a recurring subscription or one-off. "O" is one off, "R" is recurring.
                const char* ptr;
                if ((ptr = client->json.getvalue()))
                {
                    details->subscription_type = *ptr;
                }
                break;

            case MAKENAMEID6('s', 'c', 'y', 'c', 'l', 'e'):
                const char* scycle;
                if ((scycle = client->json.getvalue()))
                {
                    memcpy(details->subscription_cycle, scycle, 3);
                    details->subscription_cycle[3] = 0;
                }
                break;

            case MAKENAMEID6('s', 'r', 'e', 'n', 'e', 'w'):
            // Only provided for recurring subscriptions to indicate the best estimate of when the subscription will renew
                if (client->json.enterarray())
                {
                    details->subscription_renew = client->json.getint();
                    while(!client->json.leavearray())
                    {
                        client->json.storeobject();
                    }
                }
                break;

            case MAKENAMEID3('s', 'g', 'w'):
                if (client->json.enterarray())
                {
                    client->json.storeobject(&details->subscription_method);
                    while(!client->json.leavearray())
                    {
                        client->json.storeobject();
                    }
                }
                break;

            case MAKENAMEID6('s', 'g', 'w', 'i', 'd', 's'):
                if (client->json.enterarray())
                {
                    details->subscription_method_id = static_cast<int>(client->json.getint());
                    while (!client->json.leavearray())
                    {
                        client->json.storeobject();
                    }
                }
                break;

            case MAKENAMEID3('r', 't', 't'):
                details->transfer_hist_valid = !client->json.getint();
                break;

            case MAKENAMEID6('s', 'u', 'n', 't', 'i', 'l'):
            // Time the last active PRO plan will expire (may be different from current one)
                details->pro_until = client->json.getint();
                client->mMyAccount.setProUntil(static_cast<m_time_t>(details->pro_until));
                break;

            case MAKENAMEID7('b', 'a', 'l', 'a', 'n', 'c', 'e'):
            // Balance of your account
                if (client->json.enterarray())
                {
                    const char* cur;
                    const char* amount;

                    while (client->json.enterarray())
                    {
                        if ((amount = client->json.getvalue()) && (cur = client->json.getvalue()))
                        {
                            size_t t = details->balances.size();
                            details->balances.resize(t + 1);
                            details->balances[t].amount = atof(amount);
                            memcpy(details->balances[t].currency, cur, 3);
                            details->balances[t].currency[3] = 0;
                        }

                        client->json.leavearray();
                    }

                    client->json.leavearray();
                }
                break;

            case MAKENAMEID4('u', 's', 'l', 'w'):
            // The percentage (in 1000s) indicating the limit at which you are 'nearly' over. Currently 98% for PRO, 90% for free.
                uslw = int(client->json.getint());
                break;

            case EOO:
                assert(!mStorage || (got_storage && got_storage_used) || client->loggedinfolderlink());

                if (mStorage)
                {
                    if (uslw <= 0)
                    {
                        uslw = 9000;
                        LOG_warn << "Using default almost overstorage threshold";
                    }

                    if (details->storage_used >= details->storage_max)
                    {
                        LOG_debug << "Account full";
                        bool isPaywall = (client->ststatus == STORAGE_PAYWALL);
                        client->activateoverquota(0, isPaywall);
                    }
                    else if (details->storage_used >= (details->storage_max / 10000 * uslw))
                    {
                        LOG_debug << "Few storage space available";
                        client->setstoragestatus(STORAGE_ORANGE);
                    }
                    else
                    {
                        LOG_debug << "There are no storage problems";
                        client->setstoragestatus(STORAGE_GREEN);
                    }
                }

                if (mPro)
                {
                    // Pro level can change without a payment (ie. with coupons or by helpdesk)
                    // and in those cases, the `psts` packet is not triggered. However, the SDK
                    // should notify the app and resume transfers, etc.
                    bool changed = client->mCachedStatus.addOrUpdate(CacheableStatus::STATUS_PRO_LEVEL, details->pro_level);
                    if (changed)
                    {
                        client->app->account_updated();
                        client->abortbackoff(true);
                    }
                }

                client->app->account_details(details.get(), mStorage, mTransfer, mPro, false, false, false);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->account_details(details.get(), API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandQueryTransferQuota::CommandQueryTransferQuota(MegaClient* client, m_off_t size)
{
    cmd("qbq");
    arg("s", size);

    tag = client->reqtag;
}

bool CommandQueryTransferQuota::procresult(Result r)
{
    if (!r.wasErrorOrOK())
    {
        LOG_err << "Unexpected response: " << client->json.pos;
        client->json.storeobject();

        // Returns 0 to not alarm apps and don't show overquota pre-warnings
        // if something unexpected is received, following the same approach as
        // in the webclient
        client->app->querytransferquota_result(0);
        return false;
    }

    client->app->querytransferquota_result(r.errorOrOK());
    return true;
}

CommandGetUserTransactions::CommandGetUserTransactions(MegaClient* client, std::shared_ptr<AccountDetails> ad)
{
    cmd("utt");

    details = ad;
    tag = client->reqtag;
}

bool CommandGetUserTransactions::procresult(Result r)
{
    details->transactions.clear();

    while (client->json.enterarray())
    {
        const char* handle = client->json.getvalue();
        m_time_t ts = client->json.getint();
        const char* delta = client->json.getvalue();
        const char* cur = client->json.getvalue();

        if (handle && (ts > 0) && delta && cur)
        {
            size_t t = details->transactions.size();
            details->transactions.resize(t + 1);
            memcpy(details->transactions[t].handle, handle, 11);
            details->transactions[t].handle[11] = 0;
            details->transactions[t].timestamp = ts;
            details->transactions[t].delta = atof(delta);
            memcpy(details->transactions[t].currency, cur, 3);
            details->transactions[t].currency[3] = 0;
        }

        client->json.leavearray();
    }

    client->app->account_details(details.get(), false, false, false, false, true, false);
    return true;
}

CommandGetUserPurchases::CommandGetUserPurchases(MegaClient* client, std::shared_ptr<AccountDetails> ad)
{
    cmd("utp");

    details = ad;
    tag = client->reqtag;
}

bool CommandGetUserPurchases::procresult(Result r)
{
    client->restag = tag;

    details->purchases.clear();

    while (client->json.enterarray())
    {
        const char* handle = client->json.getvalue();
        const m_time_t ts = client->json.getint();
        const char* amount = client->json.getvalue();
        const char* cur = client->json.getvalue();
        int method = (int)client->json.getint();

        if (handle && (ts > 0) && amount && cur && (method >= 0))
        {
            size_t t = details->purchases.size();
            details->purchases.resize(t + 1);
            memcpy(details->purchases[t].handle, handle, 11);
            details->purchases[t].handle[11] = 0;
            details->purchases[t].timestamp = ts;
            details->purchases[t].amount = atof(amount);
            memcpy(details->purchases[t].currency, cur, 3);
            details->purchases[t].currency[3] = 0;
            details->purchases[t].method = method;
        }

        client->json.leavearray();
    }

    client->app->account_details(details.get(), false, false, false, true, false, false);
    return true;
}

CommandGetUserSessions::CommandGetUserSessions(MegaClient* client, std::shared_ptr<AccountDetails> ad)
{
    cmd("usl");
    arg("x", 1); // Request the additional id and alive information

    details = ad;
    tag = client->reqtag;
}

bool CommandGetUserSessions::procresult(Result r)
{
    details->sessions.clear();

    while (client->json.enterarray())
    {
        size_t t = details->sessions.size();
        details->sessions.resize(t + 1);

        details->sessions[t].timestamp = client->json.getint();
        details->sessions[t].mru = client->json.getint();
        client->json.storeobject(&details->sessions[t].useragent);
        client->json.storeobject(&details->sessions[t].ip);

        const char* country = client->json.getvalue();
        memcpy(details->sessions[t].country, country ? country : "\0\0", 2);
        details->sessions[t].country[2] = 0;

        details->sessions[t].current = (int)client->json.getint();

        details->sessions[t].id = client->json.gethandle(8);
        details->sessions[t].alive = (int)client->json.getint();

        client->json.leavearray();
    }

    client->app->account_details(details.get(), false, false, false, false, false, true);
    return true;
}

CommandSetPH::CommandSetPH(MegaClient* client, Node* n, int del, m_time_t cets, bool writable, bool megaHosted,
    int ctag, std::function<void(Error, handle, handle)> f)
{
    h = n->nodehandle;
    ets = cets;
    tag = ctag;
    mWritable = writable;
    completion = move(f);
    assert(completion);

    cmd("l");
    arg("n", (byte*)&n->nodehandle, MegaClient::NODEHANDLE);

    if (del)
    {
        arg("d", 1);
    }

    if (ets)
    {
        arg("ets", ets);
    }

    if (writable)
    {
        arg("w", "1");
    }

    if (megaHosted)
    {
        assert(n->sharekey && "attempting to share a key that is not set");
        arg("sk", n->sharekey->key, SymmCipher::KEYLENGTH);
    }
}

bool CommandSetPH::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        completion(r.errorOrOK(), UNDEF, UNDEF);
        return true;
    }

    handle ph = UNDEF;
    std::string authKey;

    if (mWritable) // aparently, depending on 'w', the response can be [{"ph":"XXXXXXXX","w":"YYYYYYYYYYYYYYYYYYYYYY"}] or simply [XXXXXXXX]
    {
        bool exit = false;
        while (!exit)
        {
            switch (client->json.getnameid())
            {
            case 'w':
                client->json.storeobject(&authKey);
                break;

            case MAKENAMEID2('p', 'h'):
                ph = client->json.gethandle();
                break;

            case EOO:
            {
                if (authKey.empty())
                {
                    completion(API_EINTERNAL, UNDEF, UNDEF);
                    return false;
                }
                exit = true;
                break;
            }
            default:
                if (!client->json.storeobject())
                {
                    completion(API_EINTERNAL, UNDEF, UNDEF);
                    return false;
                }
            }
        }
    }
    else    // format: [XXXXXXXX]
    {
        ph = client->json.gethandle();
    }

    if (ISUNDEF(ph))
    {
        completion(API_EINTERNAL, UNDEF, UNDEF);
        return false;
    }

    Node *n = client->nodebyhandle(h);
    if (n)
    {
        n->setpubliclink(ph, time(nullptr), ets, false, authKey);
        n->changed.publiclink = true;
        client->notifynode(n);
    }

    completion(API_OK, h, ph);
    return true;
}

CommandGetPH::CommandGetPH(MegaClient* client, handle cph, const byte* ckey, int cop)
{
    cmd("g");
    arg("p", (byte*)&cph, MegaClient::NODEHANDLE);

    ph = cph;
    havekey = ckey ? true : false;
    if (havekey)
    {
        memcpy(key, ckey, sizeof key);
    }
    tag = client->reqtag;
    op = cop;
}

bool CommandGetPH::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->openfilelink_result(r.errorOrOK());
        return true;
    }

    m_off_t s = -1;
    string a, fa;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 's':
                s = client->json.getint();
                break;

            case MAKENAMEID2('a', 't'):
                client->json.storeobject(&a);
                break;

            case MAKENAMEID2('f', 'a'):
                client->json.storeobject(&fa);
                break;

            case EOO:
                // we want at least the attributes
                if (s >= 0)
                {
                    a.resize(Base64::atob(a.c_str(), (byte*)a.data(), int(a.size())));

                    if (op == 2)    // importing WelcomePDF for new account
                    {
                        assert(havekey);

                        vector<NewNode> newnodes(1);
                        auto newnode = &newnodes[0];

                        // set up new node
                        newnode->source = NEW_PUBLIC;
                        newnode->type = FILENODE;
                        newnode->nodehandle = ph;
                        newnode->parenthandle = UNDEF;
                        newnode->nodekey.assign((char*)key, FILENODEKEYLENGTH);
                        newnode->attrstring.reset(new string(a));
                        client->putnodes(client->rootnodes.files, NoVersioning, move(newnodes), nullptr, 0, false);
                    }
                    else if (havekey)
                    {
                        client->app->openfilelink_result(ph, key, s, &a, &fa, op);
                    }
                    else
                    {
                        client->app->openfilelink_result(ph, NULL, s, &a, &fa, op);
                    }
                }
                else
                {
                    client->app->openfilelink_result(API_EINTERNAL);
                }
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->openfilelink_result(API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandSetMasterKey::CommandSetMasterKey(MegaClient* client, const byte* newkey, const byte *hash, int hashsize, const byte *clientrandomvalue, const char *pin, string *salt)
{
    memcpy(this->newkey, newkey, SymmCipher::KEYLENGTH);

    cmd("up");
    arg("k", newkey, SymmCipher::KEYLENGTH);
    if (clientrandomvalue)
    {
        arg("crv", clientrandomvalue, SymmCipher::KEYLENGTH);
    }
    arg("uh", hash, hashsize);
    if (pin)
    {
        arg("mfa", pin);
    }

    if (salt)
    {
        this->salt = *salt;
    }

    tag = client->reqtag;
}

bool CommandSetMasterKey::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->changepw_result(r.errorOrOK());
    }
    else
    {
        // update encrypted MK and salt for further checkups
        client->k.assign((const char *) newkey, SymmCipher::KEYLENGTH);
        client->accountsalt = salt;

        client->json.storeobject();
        client->app->changepw_result(API_OK);
    }
    return true;
}

CommandCreateEphemeralSession::CommandCreateEphemeralSession(MegaClient* client,
                                                             const byte* key,
                                                             const byte* cpw,
                                                             const byte* ssc)
{
    memcpy(pw, cpw, sizeof pw);

    cmd("up");
    arg("k", key, SymmCipher::KEYLENGTH);
    arg("ts", ssc, 2 * SymmCipher::KEYLENGTH);

    tag = client->reqtag;
}

bool CommandCreateEphemeralSession::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->ephemeralSession = false;
        client->ephemeralSessionPlusPlus = false;
        client->app->ephemeral_result(r.errorOrOK());
    }
    else
    {
        client->me = client->json.gethandle(MegaClient::USERHANDLE);
        client->uid = Base64Str<MegaClient::USERHANDLE>(client->me);
        client->resumeephemeral(client->me, pw, tag);
    }
    return true;
}

CommandResumeEphemeralSession::CommandResumeEphemeralSession(MegaClient*, handle cuh, const byte* cpw, int ctag)
{
    memcpy(pw, cpw, sizeof pw);

    uh = cuh;

    cmd("us");
    arg("user", (byte*)&uh, MegaClient::USERHANDLE);

    tag = ctag;
}

bool CommandResumeEphemeralSession::procresult(Result r)
{
    byte keybuf[SymmCipher::KEYLENGTH];
    byte sidbuf[MegaClient::SIDLEN];
    int havek = 0, havecsid = 0;

    if (r.wasErrorOrOK())
    {
        client->app->ephemeral_result(r.errorOrOK());
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'k':
                havek = client->json.storebinary(keybuf, sizeof keybuf) == sizeof keybuf;
                break;

            case MAKENAMEID4('t', 's', 'i', 'd'):
                havecsid = client->json.storebinary(sidbuf, sizeof sidbuf) == sizeof sidbuf;
                break;

            case EOO:
                if (!havek || !havecsid)
                {
                    client->app->ephemeral_result(API_EINTERNAL);
                    return false;
                }

                client->sid.assign((const char *)sidbuf, sizeof sidbuf);

                client->key.setkey(pw);
                client->key.ecb_decrypt(keybuf);

                client->key.setkey(keybuf);

                if (!client->checktsid(sidbuf, sizeof sidbuf))
                {
                    client->app->ephemeral_result(API_EKEY);
                    return true;
                }

                client->me = uh;
                client->uid = Base64Str<MegaClient::USERHANDLE>(client->me);

                client->openStatusTable(true);
                client->app->ephemeral_result(uh, pw);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->ephemeral_result(API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandCancelSignup::CommandCancelSignup(MegaClient *client)
{
    cmd("ucr");

    tag = client->reqtag;
}

bool CommandCancelSignup::procresult(Result r)
{
    client->app->cancelsignup_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandWhyAmIblocked::CommandWhyAmIblocked(MegaClient *client)
{
    cmd("whyamiblocked");
    batchSeparately = true;  // don't let any other commands that might get batched with it cause the whole batch to fail

    tag = client->reqtag;
}

bool CommandWhyAmIblocked::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_OK)) //unblocked
        {
            client->unblock();
        }

        client->app->whyamiblocked_result(r.errorOrOK());
        return true;
    }
    else if (client->json.isnumeric())
    {
         int response = int(client->json.getint());
         client->app->whyamiblocked_result(response);
         return true;
    }

    client->json.storeobject();
    client->app->whyamiblocked_result(API_EINTERNAL);
	return false;
}

CommandSendSignupLink2::CommandSendSignupLink2(MegaClient* client, const char* email, const char* name)
{
    cmd("uc2");
    arg("n", (byte*)name, int(strlen(name)));
    arg("m", (byte*)email, int(strlen(email)));
    arg("v", 2);
    tag = client->reqtag;
}

CommandSendSignupLink2::CommandSendSignupLink2(MegaClient* client, const char* email, const char* name, byte *clientrandomvalue, byte *encmasterkey, byte *hashedauthkey)
{
    cmd("uc2");
    arg("n", (byte*)name, int(strlen(name)));
    arg("m", (byte*)email, int(strlen(email)));
    arg("crv", clientrandomvalue, SymmCipher::KEYLENGTH);
    arg("hak", hashedauthkey, SymmCipher::KEYLENGTH);
    arg("k", encmasterkey, SymmCipher::KEYLENGTH);
    arg("v", 2);

    tag = client->reqtag;
}

bool CommandSendSignupLink2::procresult(Result r)
{
    client->app->sendsignuplink_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandConfirmSignupLink2::CommandConfirmSignupLink2(MegaClient* client,
                                                   const byte* code,
                                                   unsigned len)
{
    cmd("ud2");
    arg("c", code, len);

    tag = client->reqtag;
}

bool CommandConfirmSignupLink2::procresult(Result r)
{
    string name;
    string email;
    handle uh = UNDEF;
    int version = 0;

    if (r.wasErrorOrOK())
    {
        client->app->confirmsignuplink2_result(UNDEF, NULL, NULL, r.errorOrOK());
        return true;
    }

    assert(r.hasJsonArray());
    if (client->json.storebinary(&email) && client->json.storebinary(&name))
    {
        uh = client->json.gethandle(MegaClient::USERHANDLE);
        version = int(client->json.getint());
    }
    while (client->json.storeobject());

    if (!ISUNDEF(uh) && version == 2)
    {
        client->ephemeralSession = false;
        client->app->confirmsignuplink2_result(uh, name.c_str(), email.c_str(), API_OK);
        return true;
    }
    else
    {
        client->app->confirmsignuplink2_result(UNDEF, NULL, NULL, API_EINTERNAL);
        return false;
    }
}

CommandSetKeyPair::CommandSetKeyPair(MegaClient* client, const byte* privk,
                                     unsigned privklen, const byte* pubk,
                                     unsigned pubklen)
{
    cmd("up");
    arg("privk", privk, privklen);
    arg("pubk", pubk, pubklen);

    tag = client->reqtag;

    len = privklen;
    privkBuffer.reset(new byte[privklen]);
    memcpy(privkBuffer.get(), privk, len);
}

bool CommandSetKeyPair::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->setkeypair_result(r.errorOrOK());
        return true;
    }

    client->json.storeobject();

    client->key.ecb_decrypt(privkBuffer.get(), len);
    client->mPrivKey.resize(AsymmCipher::MAXKEYLENGTH * 2);
    client->mPrivKey.resize(Base64::btoa(privkBuffer.get(), len, (char *)client->mPrivKey.data()));

    client->app->setkeypair_result(API_OK);
    return true;
}

// fetch full node tree
CommandFetchNodes::CommandFetchNodes(MegaClient* client, int tag, bool nocache)
{
    cmd("f");
    arg("c", 1);
    arg("r", 1);

    if (!nocache)
    {
        arg("ca", 1);
    }

    // The servers are more efficient with this command when it's the only one in the batch
    batchSeparately = true;

    this->tag = tag;
}

// purge and rebuild node/user tree
bool CommandFetchNodes::procresult(Result r)
{
    WAIT_CLASS::bumpds();
    client->fnstats.timeToLastByte = Waiter::ds - client->fnstats.startTime;

    client->purgenodesusersabortsc(true);

    if (r.wasErrorOrOK())
    {
        client->fetchingnodes = false;
        client->app->fetchnodes_result(r.errorOrOK());
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'f':
                // nodes
                if (!client->readnodes(&client->json, 0, PUTNODES_APP, nullptr, 0, false))
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }
                break;

            case MAKENAMEID2('f', '2'):
                // old versions
                if (!client->readnodes(&client->json, 0, PUTNODES_APP, nullptr, 0, false))
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }
                break;

            case MAKENAMEID2('o', 'k'):
                // outgoing sharekeys
                client->readok(&client->json);
                break;

            case 's':
                // Fall through
            case MAKENAMEID2('p', 's'):
                // outgoing or pending shares
                client->readoutshares(&client->json);
                break;

            case 'u':
                // users/contacts
                if (!client->readusers(&client->json, false))
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }
                break;

            case MAKENAMEID2('c', 'r'):
                // crypto key request
                client->proccr(&client->json);
                break;

            case MAKENAMEID2('s', 'r'):
                // sharekey distribution request
                client->procsr(&client->json);
                break;

            case MAKENAMEID2('s', 'n'):
                // sequence number
                if (!client->scsn.setScsn(&client->json))
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }
                break;

            case MAKENAMEID3('i', 'p', 'c'):
                // Incoming pending contact
                client->readipc(&client->json);
                break;

            case MAKENAMEID3('o', 'p', 'c'):
                // Outgoing pending contact
                client->readopc(&client->json);
                break;

            case MAKENAMEID2('p', 'h'):
                // Public links handles
                client->procph(&client->json);
                break;

            case MAKENAMEID4('a', 'e', 's', 'p'):
                // Sets and Elements
                client->procaesp(); // continue even if it failed, it's not critical
                break;

#ifdef ENABLE_CHAT
            case MAKENAMEID3('m', 'c', 'f'):
                // List of chatrooms
                client->procmcf(&client->json);
                break;

            case MAKENAMEID5('m', 'c', 'p', 'n', 'a'):   // fall-through
            case MAKENAMEID4('m', 'c', 'n', 'a'):
                // nodes shared in chatrooms
                client->procmcna(&client->json);
                break;
#endif
            case EOO:
            {
                if (!client->scsn.ready())
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }

                client->mergenewshares(0);
                client->applykeys();
                client->initsc();
                client->pendingsccommit = false;
                client->fetchnodestag = tag;

                WAIT_CLASS::bumpds();
                client->fnstats.timeToCached = Waiter::ds - client->fnstats.startTime;
                client->fnstats.nodesCached = client->nodes.size();

                return true;
            }
            default:
                if (!client->json.storeobject())
                {
                    client->fetchingnodes = false;
                    client->app->fetchnodes_result(API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandSubmitPurchaseReceipt::CommandSubmitPurchaseReceipt(MegaClient *client, int type, const char *receipt, handle lph, int phtype, int64_t ts)
{
    cmd("vpay");
    arg("t", type);

    if(receipt)
    {
        arg("receipt", receipt);
    }

    if(type == 2 && client->loggedin() == FULLACCOUNT)
    {
        arg("user", client->finduser(client->me)->uid.c_str());
    }

    if (!ISUNDEF(lph))
    {
        if (phtype == 0) // legacy mode
        {
            arg("aff", (byte*)&lph, MegaClient::NODEHANDLE);
        }
        else
        {
            beginobject("aff");
            arg("id", (byte*)&lph, MegaClient::NODEHANDLE);
            arg("ts", ts);
            arg("t", phtype);   // 1=affiliate id, 2=file/folder link, 3=chat link, 4=contact link
            endobject();
        }
    }

    tag = client->reqtag;
}

bool CommandSubmitPurchaseReceipt::procresult(Result r)
{
    client->app->submitpurchasereceipt_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

// Credit Card Store
CommandCreditCardStore::CommandCreditCardStore(MegaClient* client, const char *cc, const char *last4, const char *expm, const char *expy, const char *hash)
{
    cmd("ccs");
    arg("cc", cc);
    arg("last4", last4);
    arg("expm", expm);
    arg("expy", expy);
    arg("hash", hash);

    tag = client->reqtag;
}

bool CommandCreditCardStore::procresult(Result r)
{
    client->app->creditcardstore_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandCreditCardQuerySubscriptions::CommandCreditCardQuerySubscriptions(MegaClient* client)
{
    cmd("ccqns");

    tag = client->reqtag;
}

bool CommandCreditCardQuerySubscriptions::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->creditcardquerysubscriptions_result(0, r.errorOrOK());
        return true;
    }
    else if (client->json.isnumeric())
    {
        int number = int(client->json.getint());
        client->app->creditcardquerysubscriptions_result(number, API_OK);
        return true;
    }
    else
    {
        client->json.storeobject();
        client->app->creditcardquerysubscriptions_result(0, API_EINTERNAL);
        return false;
    }
}

CommandCreditCardCancelSubscriptions::CommandCreditCardCancelSubscriptions(MegaClient* client, const char* reason)
{
    cmd("cccs");

    if (reason)
    {
        arg("r", reason);
    }

    tag = client->reqtag;
}

bool CommandCreditCardCancelSubscriptions::procresult(Result r)
{
    client->app->creditcardcancelsubscriptions_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandCopySession::CommandCopySession(MegaClient *client)
{
    cmd("us");
    arg("c", 1);
    batchSeparately = true;  // don't let any other commands that might get batched with it cause the whole batch to fail when blocked
    tag = client->reqtag;
}

// for ephemeral accounts, it returns "tsid" instead of "csid" -> not supported, will return API_EINTERNAL
bool CommandCopySession::procresult(Result r)
{
    string session;
    byte sidbuf[AsymmCipher::MAXKEYLENGTH];
    int len_csid = 0;

    if (r.wasErrorOrOK())
    {
        assert(r.errorOrOK() != API_OK); // API shouldn't return OK, but a session
        client->app->copysession_result(NULL, r.errorOrOK());
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID4('c', 's', 'i', 'd'):
                len_csid = client->json.storebinary(sidbuf, sizeof sidbuf);
                break;

            case EOO:
                if (len_csid < 32)
                {
                    client->app->copysession_result(NULL, API_EINTERNAL);
                    return false;
                }

                if (!client->asymkey.decrypt(sidbuf, len_csid, sidbuf, MegaClient::SIDLEN))
                {
                    client->app->copysession_result(NULL, API_EINTERNAL);
                    return false;
                }

                session.resize(MegaClient::SIDLEN * 4 / 3 + 4);
                session.resize(Base64::btoa(sidbuf, MegaClient::SIDLEN, (char *)session.data()));
                client->app->copysession_result(&session, API_OK);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->copysession_result(NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandGetPaymentMethods::CommandGetPaymentMethods(MegaClient *client)
{
    cmd("ufpq");
    tag = client->reqtag;
}

bool CommandGetPaymentMethods::procresult(Result r)
{
    int methods = 0;
    int64_t value;

    if (r.wasErrorOrOK())
    {
        if (!r.wasError(API_OK))
        {
            client->app->getpaymentmethods_result(methods, r.errorOrOK());

            //Consume remaining values if they exist
            while(client->json.isnumeric())
            {
                client->json.getint();
            }
            return true;
        }

        value = static_cast<int64_t>(error(r.errorOrOK()));
    }
    else if (client->json.isnumeric())
    {
        value = client->json.getint();
    }
    else
    {
        LOG_err << "Parse error in ufpq";
        client->app->getpaymentmethods_result(methods, API_EINTERNAL);
        return false;
    }

    methods |= 1 << value;

    while (client->json.isnumeric())
    {
        value = client->json.getint();
        if (value < 0)
        {
            client->app->getpaymentmethods_result(methods, static_cast<error>(value));

            //Consume remaining values if they exist
            while(client->json.isnumeric())
            {
                client->json.getint();
            }
            return true;
        }

        methods |= 1 << value;
    }

    client->app->getpaymentmethods_result(methods, API_OK);
    return true;
}

CommandSendReport::CommandSendReport(MegaClient *client, const char *type, const char *blob, const char *uid)
{
    cmd("clog");

    arg("t", type);

    if (blob)
    {
        arg("d", blob);
    }

    if (uid)
    {
        arg("id", uid);
    }

    tag = client->reqtag;
}

bool CommandSendReport::procresult(Result r)
{
    client->app->userfeedbackstore_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandSendEvent::CommandSendEvent(MegaClient *client, int type, const char *desc)
{
    cmd("log");
    arg("e", type);
    arg("m", desc);

    tag = client->reqtag;
}

bool CommandSendEvent::procresult(Result r)
{
    client->app->sendevent_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandSupportTicket::CommandSupportTicket(MegaClient *client, const char *message, int type)
{
    cmd("sse");
    arg("t", type);
    arg("b", 1);    // base64 encoding for `msg`
    arg("m", (const byte*)message, int(strlen(message)));

    tag = client->reqtag;
}

bool CommandSupportTicket::procresult(Result r)
{
    client->app->supportticket_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandCleanRubbishBin::CommandCleanRubbishBin(MegaClient *client)
{
    cmd("dr");

    tag = client->reqtag;
}

bool CommandCleanRubbishBin::procresult(Result r)
{
    client->app->cleanrubbishbin_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandGetRecoveryLink::CommandGetRecoveryLink(MegaClient *client, const char *email, int type, const char *pin)
{
    cmd("erm");
    arg("m", email);
    arg("t", type);

    if (type == CANCEL_ACCOUNT && pin)
    {
        arg("mfa", pin);
    }

    tag = client->reqtag;
}

bool CommandGetRecoveryLink::procresult(Result r)
{
    client->app->getrecoverylink_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandQueryRecoveryLink::CommandQueryRecoveryLink(MegaClient *client, const char *linkcode)
{
    cmd("erv");
    arg("c", linkcode);

    tag = client->reqtag;
}

bool CommandQueryRecoveryLink::procresult(Result r)
{
    // [<code>,"<email>","<ip_address>",<timestamp>,"<user_handle>",["<email>"]]   (and we are already in the array)
    string email;
    string ip;
    m_time_t ts;
    handle uh;

    if (r.wasStrictlyError())
    {
        client->app->queryrecoverylink_result(r.errorOrOK());
        return true;
    }

    if (!client->json.isnumeric())
    {
        client->app->queryrecoverylink_result(API_EINTERNAL);
        return false;
    }

    int type = static_cast<int>(client->json.getint());

    if ( !client->json.storeobject(&email)  ||
         !client->json.storeobject(&ip)     ||
         ((ts = client->json.getint()) == -1) ||
         !(uh = client->json.gethandle(MegaClient::USERHANDLE)) )
    {
        client->app->queryrecoverylink_result(API_EINTERNAL);
        return false;
    }

    string tmp;
    vector<string> emails;

    // read emails registered for this account
    client->json.enterarray();
    while (client->json.storeobject(&tmp))
    {
        emails.push_back(tmp);
        if (*client->json.pos == ']')
        {
            break;
        }
    }
    client->json.leavearray();  // emails array

    if (!emails.size()) // there should be at least one email
    {
        client->app->queryrecoverylink_result(API_EINTERNAL);
        return false;
    }

    if (client->loggedin() == FULLACCOUNT && uh != client->me)
    {
        client->app->queryrecoverylink_result(API_EACCESS);
        return true;
    }

    client->app->queryrecoverylink_result(type, email.c_str(), ip.c_str(), time_t(ts), uh, &emails);
    return true;
}

CommandGetPrivateKey::CommandGetPrivateKey(MegaClient *client, const char *code)
{
    cmd("erx");
    arg("r", "gk");
    arg("c", code);

    tag = client->reqtag;
}

bool CommandGetPrivateKey::procresult(Result r)
{
    if (r.wasErrorOrOK())   // error
    {
        client->app->getprivatekey_result(r.errorOrOK());
        return true;
    }
    else
    {
        byte privkbuf[AsymmCipher::MAXKEYLENGTH * 2];
        int len_privk = client->json.storebinary(privkbuf, sizeof privkbuf);

        // account has RSA keypair: decrypt server-provided session ID
        if (len_privk < 256)
        {
            client->app->getprivatekey_result(API_EINTERNAL);
            return false;
        }
        else
        {
            client->app->getprivatekey_result((error)API_OK, privkbuf, len_privk);
            return true;
        }
    }
}

CommandConfirmRecoveryLink::CommandConfirmRecoveryLink(MegaClient *client, const char *code, const byte *hash, int hashsize, const byte *clientrandomvalue, const byte *encMasterKey, const byte *initialSession)
{
    cmd("erx");

    if (!initialSession)
    {
        arg("r", "sk");
    }

    arg("c", code);

    arg("x", encMasterKey, SymmCipher::KEYLENGTH);
    if (!clientrandomvalue)
    {
        arg("y", hash, hashsize);
    }
    else
    {
        beginobject("y");
        arg("crv", clientrandomvalue, SymmCipher::KEYLENGTH);
        arg("hak", hash, hashsize); //hashed authentication key
        endobject();
    }

    if (initialSession)
    {
        arg("z", initialSession, 2 * SymmCipher::KEYLENGTH);
    }

    tag = client->reqtag;
}

bool CommandConfirmRecoveryLink::procresult(Result r)
{
    client->app->confirmrecoverylink_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandConfirmCancelLink::CommandConfirmCancelLink(MegaClient *client, const char *code)
{
    cmd("erx");
    arg("c", code);

    tag = client->reqtag;
}

bool CommandConfirmCancelLink::procresult(Result r)
{
    MegaApp *app = client->app;
    app->confirmcancellink_result(r.errorOrOK());
    if (r.wasError(API_OK))
    {
        app->request_error(API_ESID);
    }
    return r.wasErrorOrOK();
}

CommandResendVerificationEmail::CommandResendVerificationEmail(MegaClient *client)
{
    cmd("era");
    batchSeparately = true;  // don't let any other commands that might get batched with it cause the whole batch to fail

    tag = client->reqtag;
}

bool CommandResendVerificationEmail::procresult(Result r)
{
    client->app->resendverificationemail_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandResetSmsVerifiedPhoneNumber::CommandResetSmsVerifiedPhoneNumber(MegaClient *client)
{
    cmd("smsr");
    tag = client->reqtag;
}

bool CommandResetSmsVerifiedPhoneNumber::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        client->mSmsVerifiedPhone.clear();
    }
    client->app->resetSmsVerifiedPhoneNumber_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandValidatePassword::CommandValidatePassword(MegaClient *client, const char *email, uint64_t emailhash)
{
    cmd("us");
    arg("user", email);
    arg("uh", (byte*)&emailhash, sizeof emailhash);

    tag = client->reqtag;
}

bool CommandValidatePassword::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        client->app->validatepassword_result(r.errorOrOK());
        return true;
    }
    else
    {
        assert(r.hasJsonObject());  // we don't use the object contents, and will exit the object automatically
        client->app->validatepassword_result(API_OK);
        return r.hasJsonObject();
    }
}

CommandGetEmailLink::CommandGetEmailLink(MegaClient *client, const char *email, int add, const char *pin)
{
    cmd("se");

    if (add)
    {
        arg("aa", "a");     // add
    }
    else
    {
        arg("aa", "r");     // remove
    }
    arg("e", email);
    if (pin)
    {
        arg("mfa", pin);
    }

    notself(client);

    tag = client->reqtag;
}

bool CommandGetEmailLink::procresult(Result r)
{
    client->app->getemaillink_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandConfirmEmailLink::CommandConfirmEmailLink(MegaClient *client, const char *code, const char *email, const byte *newLoginHash, bool replace)
{
    this->email = email;
    this->replace = replace;

    cmd("sec");

    arg("c", code);
    arg("e", email);
    if (newLoginHash)
    {
        arg("uh", newLoginHash, sizeof(uint64_t));
    }
    if (replace)
    {
        arg("r", 1);    // replace the current email address by this one
    }
    notself(client);

    tag = client->reqtag;
}

bool CommandConfirmEmailLink::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        User *u = client->finduser(client->me);

        if (replace)
        {
            LOG_debug << "Email changed from `" << u->email << "` to `" << email << "`";

            client->mapuser(u->userhandle, email.c_str()); // update email used as index for user's map
            u->changed.email = true;
            client->notifyuser(u);
        }
        // TODO: once we manage multiple emails, add the new email to the list of emails
    }

    client->app->confirmemaillink_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandGetVersion::CommandGetVersion(MegaClient *client, const char *appKey)
{
    this->client = client;
    cmd("lv");
    arg("a", appKey);
    tag = client->reqtag;
}

bool CommandGetVersion::procresult(Result r)
{
    int versioncode = 0;
    string versionstring;

    if (r.wasErrorOrOK())
    {
        client->app->getversion_result(0, NULL, r.errorOrOK());
        return r.wasErrorOrOK();
    }

    assert(r.hasJsonObject());
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'c':
                versioncode = int(client->json.getint());
                break;

            case 's':
                client->json.storeobject(&versionstring);
                break;

            case EOO:
                client->app->getversion_result(versioncode, versionstring.c_str(), API_OK);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    client->app->getversion_result(0, NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandGetLocalSSLCertificate::CommandGetLocalSSLCertificate(MegaClient *client)
{
    this->client = client;
    cmd("lc");
    arg("v", 1);

    tag = client->reqtag;
}

bool CommandGetLocalSSLCertificate::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getlocalsslcertificate_result(0, NULL, r.errorOrOK());
        return true;
    }

    assert(r.hasJsonObject());
    string certdata;
    m_time_t ts = 0;
    int numelements = 0;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 't':
            {
                ts = client->json.getint();
                break;
            }
            case 'd':
            {
                string data;
                client->json.enterarray();
                while (client->json.storeobject(&data))
                {
                    if (numelements)
                    {
                        certdata.append(";");
                    }
                    numelements++;
                    certdata.append(data);
                }
                client->json.leavearray();
                break;
            }
            case EOO:
            {
                if (numelements < 2)
                {
                    client->app->getlocalsslcertificate_result(0, NULL, API_EINTERNAL);
                    return false;
                }
                client->app->getlocalsslcertificate_result(ts, &certdata, API_OK);
                return true;
            }

            default:
                if (!client->json.storeobject())
                {
                    client->app->getlocalsslcertificate_result(0, NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

#ifdef ENABLE_CHAT
CommandChatCreate::CommandChatCreate(MegaClient* client, bool group, bool publicchat, const userpriv_vector* upl, const string_map* ukm, const char* title, bool meetingRoom, int chatOptions)
{
    this->client = client;
    this->chatPeers = new userpriv_vector(*upl);
    this->mPublicChat = publicchat;
    this->mTitle = title ? string(title) : "";
    this->mUnifiedKey = "";
    mMeeting = meetingRoom;


    cmd("mcc");
    arg("g", (group) ? 1 : 0);

    if (group && title)
    {
        arg("ct", title);
    }

    if (publicchat)
    {
        arg("m", 1);

        char ownHandleB64[12];
        Base64::btoa((byte *)&client->me, MegaClient::USERHANDLE, ownHandleB64);
        ownHandleB64[11] = '\0';

        string_map::const_iterator it = ukm->find(ownHandleB64);
        if (it != ukm->end())
        {
            mUnifiedKey = it->second;
            arg("ck", mUnifiedKey.c_str());
        }
    }

    if (meetingRoom)
    {
        arg("mr", 1);
    }

    if (group)
    {
        mChatOptions.set(static_cast<ChatOptions_t>(chatOptions));
        if (mChatOptions.speakRequest()) {arg("sr", 1);}
        if (mChatOptions.waitingRoom())  {arg("w", 1);}
        if (mChatOptions.openInvite())   {arg("oi", 1);}
    }

    beginarray("u");

    userpriv_vector::iterator itupl;
    for (itupl = chatPeers->begin(); itupl != chatPeers->end(); itupl++)
    {
        beginobject();

        handle uh = itupl->first;
        privilege_t priv = itupl->second;

        arg("u", (byte *)&uh, MegaClient::USERHANDLE);
        arg("p", priv);

        if (publicchat)
        {
            char uid[12];
            Base64::btoa((byte*)&uh, MegaClient::USERHANDLE, uid);
            uid[11] = '\0';

            string_map::const_iterator ituk = ukm->find(uid);
            if(ituk != ukm->end())
            {
                arg("ck", ituk->second.c_str());
            }
        }
        endobject();
    }

    endarray();

    arg("v", 1);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatCreate::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->chatcreate_result(NULL, r.errorOrOK());
        delete chatPeers;
        return true;
    }
    else
    {
        handle chatid = UNDEF;
        int shard = -1;
        bool group = false;
        m_time_t ts = -1;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case MAKENAMEID2('i','d'):
                    chatid = client->json.gethandle(MegaClient::CHATHANDLE);
                    break;

                case MAKENAMEID2('c','s'):
                    shard = int(client->json.getint());
                    break;

                case 'g':
                    group = client->json.getint();
                    break;

                case MAKENAMEID2('t', 's'):  // actual creation timestamp
                    ts = client->json.getint();
                    break;

                case EOO:
                    if (chatid != UNDEF && shard != -1)
                    {
                        if (client->chats.find(chatid) == client->chats.end())
                        {
                            client->chats[chatid] = new TextChat();
                        }

                        TextChat *chat = client->chats[chatid];
                        chat->id = chatid;
                        chat->priv = PRIV_MODERATOR;
                        chat->shard = shard;
                        delete chat->userpriv;  // discard any existing `userpriv`
                        chat->userpriv = this->chatPeers;
                        chat->group = group;
                        chat->ts = (ts != -1) ? ts : 0;
                        chat->publicchat = mPublicChat;
                        chat->meeting = mMeeting;

                        if (group) // we are creating a chat, so we need to initialize all chat options enabled/disabled
                        {
                            chat->addOrUpdateChatOptions(mChatOptions.speakRequest(), mChatOptions.waitingRoom(), mChatOptions.openInvite());
                        }

                        chat->setTag(tag ? tag : -1);
                        if (chat->group && !mTitle.empty())
                        {
                            chat->title = mTitle;
                        }
                        if (mPublicChat)
                        {
                            chat->unifiedKey = mUnifiedKey;
                        }

                        client->notifychat(chat);
                        client->app->chatcreate_result(chat, API_OK);
                    }
                    else
                    {
                        client->app->chatcreate_result(NULL, API_EINTERNAL);
                        delete chatPeers;   // unused, but might be set at creation
                    }
                    return true;

                default:
                    if (!client->json.storeobject())
                    {
                        client->app->chatcreate_result(NULL, API_EINTERNAL);
                        delete chatPeers;   // unused, but might be set at creation
                        return false;
                    }
            }
        }
    }
}

CommandSetChatOptions::CommandSetChatOptions(MegaClient* client, handle chatid, int option, bool enabled, CommandSetChatOptionsCompletion completion)
    : mCompletion(completion)
{
    this->client = client;
    mChatid = chatid;
    mOption = option;
    mEnabled = enabled;

    cmd("mco");
    arg("cid", (byte*)&chatid, MegaClient::CHATHANDLE);
    switch (option)
    {
        case ChatOptions::kOpenInvite:      arg("oi", enabled);  break;
        case ChatOptions::kSpeakRequest:    arg("sr", enabled);  break;
        case ChatOptions::kWaitingRoom:     arg("w", enabled);   break;
        default:                                                 break;
    }

    notself(client); // set i param to ignore action packet generated by our own action
    tag = client->reqtag;
}

bool CommandSetChatOptions::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        auto it = client->chats.find(mChatid);
        if (it == client->chats.end())
        {
            mCompletion(API_EINTERNAL);
            return false;
        }

        // chat options: [-1 (not updated) | 0 (remove) | 1 (add)]
        int speakRequest = mOption == ChatOptions::kSpeakRequest ? mEnabled : -1;
        int waitingRoom  = mOption == ChatOptions::kWaitingRoom  ? mEnabled : -1;
        int openInvite   = mOption == ChatOptions::kOpenInvite   ? mEnabled : -1;

        TextChat* chat = it->second;
        chat->addOrUpdateChatOptions(speakRequest, waitingRoom, openInvite);
        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    mCompletion(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatInvite::CommandChatInvite(MegaClient *client, handle chatid, handle uh, privilege_t priv, const char *unifiedkey, const char* title)
{
    this->client = client;
    this->chatid = chatid;
    this->uh = uh;
    this->priv = priv;
    this->title = title ? string(title) : "";

    cmd("mci");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("u", (byte *)&uh, MegaClient::USERHANDLE);
    arg("p", priv);
    arg("v", 1);

    if (title)
    {
        arg("ct", title);
    }

    if (unifiedkey)
    {
        arg("ck", unifiedkey);
    }

    notself(client);

    tag = client->reqtag;
}

bool CommandChatInvite::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the invitation succeed for a non-existing chatroom
            client->app->chatinvite_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        if (!chat->userpriv)
        {
            chat->userpriv = new userpriv_vector();
        }

        chat->userpriv->push_back(userpriv_pair(uh, priv));

        if (!title.empty())  // only if title was set for this chatroom, update it
        {
            chat->title = title;
        }

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatinvite_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatRemove::CommandChatRemove(MegaClient *client, handle chatid, handle uh)
{
    this->client = client;
    this->chatid = chatid;
    this->uh = uh;

    cmd("mcr");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);

    if (uh != client->me)
    {
        arg("u", (byte *)&uh, MegaClient::USERHANDLE);
    }
    arg("v", 1);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatRemove::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the invitation succeed for a non-existing chatroom
            client->app->chatremove_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        if (chat->userpriv)
        {
            userpriv_vector::iterator upvit;
            for (upvit = chat->userpriv->begin(); upvit != chat->userpriv->end(); upvit++)
            {
                if (upvit->first == uh)
                {
                    chat->userpriv->erase(upvit);
                    if (chat->userpriv->empty())
                    {
                        delete chat->userpriv;
                        chat->userpriv = NULL;
                    }
                    break;
                }
            }
        }
        else
        {
            if (uh != client->me)
            {
                // the removal succeed, but the list of peers is empty
                client->app->chatremove_result(API_EINTERNAL);
                return true;
            }
        }

        if (uh == client->me)
        {
            chat->priv = PRIV_RM;

            // clear the list of peers (if re-invited, peers will be re-added)
            delete chat->userpriv;
            chat->userpriv = NULL;
        }

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatremove_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatURL::CommandChatURL(MegaClient *client, handle chatid)
{
    this->client = client;

    cmd("mcurl");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("v", 1);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatURL::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->chaturl_result(NULL, r.errorOrOK());
        return true;
    }
    else
    {
        string url;
        if (!client->json.storeobject(&url))
        {
            client->app->chaturl_result(NULL, API_EINTERNAL);
            return false;
        }
        else
        {
            client->app->chaturl_result(&url, API_OK);
            return true;
        }
    }
}

CommandChatGrantAccess::CommandChatGrantAccess(MegaClient *client, handle chatid, handle h, const char *uid)
{
    this->client = client;
    this->chatid = chatid;
    this->h = h;
    Base64::atob(uid, (byte*)&uh, MegaClient::USERHANDLE);

    cmd("mcga");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("n", (byte*)&h, MegaClient::NODEHANDLE);
    arg("u", uid);
    arg("v", 1);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatGrantAccess::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the action succeed for a non-existing chatroom??
            client->app->chatgrantaccess_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        chat->setNodeUserAccess(h, uh);

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatgrantaccess_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatRemoveAccess::CommandChatRemoveAccess(MegaClient *client, handle chatid, handle h, const char *uid)
{
    this->client = client;
    this->chatid = chatid;
    this->h = h;
    Base64::atob(uid, (byte*)&uh, MegaClient::USERHANDLE);

    cmd("mcra");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("n", (byte*)&h, MegaClient::NODEHANDLE);
    arg("u", uid);
    arg("v", 1);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatRemoveAccess::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the action succeed for a non-existing chatroom??
            client->app->chatremoveaccess_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        chat->setNodeUserAccess(h, uh, true);

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatremoveaccess_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatUpdatePermissions::CommandChatUpdatePermissions(MegaClient *client, handle chatid, handle uh, privilege_t priv)
{
    this->client = client;
    this->chatid = chatid;
    this->uh = uh;
    this->priv = priv;

    cmd("mcup");
    arg("v", 1);

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("u", (byte *)&uh, MegaClient::USERHANDLE);
    arg("p", priv);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatUpdatePermissions::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the invitation succeed for a non-existing chatroom
            client->app->chatupdatepermissions_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        if (uh != client->me)
        {
            if (!chat->userpriv)
            {
                // the update succeed, but that peer is not included in the chatroom
                client->app->chatupdatepermissions_result(API_EINTERNAL);
                return true;
            }

            bool found = false;
            userpriv_vector::iterator upvit;
            for (upvit = chat->userpriv->begin(); upvit != chat->userpriv->end(); upvit++)
            {
                if (upvit->first == uh)
                {
                    chat->userpriv->erase(upvit);
                    chat->userpriv->push_back(userpriv_pair(uh, priv));
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                // the update succeed, but that peer is not included in the chatroom
                client->app->chatupdatepermissions_result(API_EINTERNAL);
                return true;
            }
        }
        else
        {
            chat->priv = priv;
        }

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatupdatepermissions_result(r.errorOrOK());
    return r.wasErrorOrOK();
}


CommandChatTruncate::CommandChatTruncate(MegaClient *client, handle chatid, handle messageid)
{
    this->client = client;
    this->chatid = chatid;

    cmd("mct");
    arg("v", 1);

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("m", (byte*)&messageid, MegaClient::CHATHANDLE);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatTruncate::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the truncation succeed for a non-existing chatroom
            client->app->chattruncate_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chattruncate_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatSetTitle::CommandChatSetTitle(MegaClient *client, handle chatid, const char *title)
{
    this->client = client;
    this->chatid = chatid;
    this->title = title ? string(title) : "";

    cmd("mcst");
    arg("v", 1);

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("ct", title);
    notself(client);

    tag = client->reqtag;
}

bool CommandChatSetTitle::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        if (client->chats.find(chatid) == client->chats.end())
        {
            // the invitation succeed for a non-existing chatroom
            client->app->chatsettitle_result(API_EINTERNAL);
            return true;
        }

        TextChat *chat = client->chats[chatid];
        chat->title = title;

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatsettitle_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatPresenceURL::CommandChatPresenceURL(MegaClient *client)
{
    this->client = client;
    cmd("pu");
    notself(client);
    tag = client->reqtag;
}

bool CommandChatPresenceURL::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->chatpresenceurl_result(NULL, r.errorOrOK());
        return true;
    }
    else
    {
        string url;
        if (!client->json.storeobject(&url))
        {
            client->app->chatpresenceurl_result(NULL, API_EINTERNAL);
            return false;
        }
        else
        {
            client->app->chatpresenceurl_result(&url, API_OK);
            return true;
        }
    }
}

CommandRegisterPushNotification::CommandRegisterPushNotification(MegaClient *client, int deviceType, const char *token)
{
    this->client = client;
    cmd("spt");
    arg("p", deviceType);
    arg("t", token);

    tag = client->reqtag;
}

bool CommandRegisterPushNotification::procresult(Result r)
{
    client->app->registerpushnotification_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandArchiveChat::CommandArchiveChat(MegaClient *client, handle chatid, bool archive)
{
    this->mChatid = chatid;
    this->mArchive = archive;

    cmd("mcsf");

    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("m", 1);
    arg("f", archive);

    notself(client);

    tag = client->reqtag;
}

bool CommandArchiveChat::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        textchat_map::iterator it = client->chats.find(mChatid);
        if (it == client->chats.end())
        {
            LOG_err << "Archive chat succeeded for a non-existing chatroom";
            client->app->archivechat_result(API_ENOENT);
            return true;
        }

        TextChat *chat = it->second;
        chat->setFlag(mArchive, TextChat::FLAG_OFFSET_ARCHIVE);

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->archivechat_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandSetChatRetentionTime::CommandSetChatRetentionTime(MegaClient *client, handle chatid, unsigned period)
{
    mChatid = chatid;

    cmd("mcsr");
    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("d", period);
    arg("ds", 1);
    tag = client->reqtag;
}

bool CommandSetChatRetentionTime::procresult(Result r)
{
    client->app->setchatretentiontime_result(r.errorOrOK());
    return true;
}

CommandRichLink::CommandRichLink(MegaClient *client, const char *url)
{
    cmd("erlsd");

    arg("url", url);

    tag = client->reqtag;
}

bool CommandRichLink::procresult(Result r)
{
    // error format: [{"error":<code>}]
    // result format: [{"result":{
    //                      "url":"<url>",
    //                      "t":"<title>",
    //                      "d":"<description>",
    //                      "ic":"<format>:<icon_B64>",
    //                      "i":"<format>:<image>"}}]

    if (r.wasErrorOrOK())
    {
        client->app->richlinkrequest_result(NULL, r.errorOrOK());
        return true;
    }


    string res;
    int errCode = 0;
    string metadata;
    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID5('e', 'r', 'r', 'o', 'r'):
                errCode = int(client->json.getint());
                break;

            case MAKENAMEID6('r', 'e', 's', 'u', 'l', 't'):
                client->json.storeobject(&metadata);
                break;

            case EOO:
            {
                error e = API_EINTERNAL;
                if (!metadata.empty())
                {
                    client->app->richlinkrequest_result(&metadata, API_OK);
                    return true;
                }
                else if (errCode)
                {
                    switch(errCode)
                    {
                        case 403:
                            e = API_EACCESS;
                            break;

                        case 404:
                            e = API_ENOENT;
                            break;

                        default:
                            e = API_EINTERNAL;
                            break;
                    }
                }

                client->app->richlinkrequest_result(NULL, e);
                return true;
            }

            default:
                if (!client->json.storeobject())
                {
                    client->app->richlinkrequest_result(NULL, API_EINTERNAL);
                    return false;
                }
        }
    }
}

CommandChatLink::CommandChatLink(MegaClient *client, handle chatid, bool del, bool createifmissing)
{
    mDelete = del;

    cmd("mcph");
    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);

    if (del)
    {
        arg("d", 1);
    }

    if (!createifmissing)
    {
        arg("cim", (m_off_t)0);
    }

    notself(client);
    tag = client->reqtag;
}

bool CommandChatLink::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        if (r.wasError(API_OK) && !mDelete)
        {
            LOG_err << "Unexpected response for create/get chatlink";
            client->app->chatlink_result(UNDEF, API_EINTERNAL);
            return true;
        }

        client->app->chatlink_result(UNDEF, r.errorOrOK());
        return true;
    }
    else
    {
        handle h = client->json.gethandle(MegaClient::CHATLINKHANDLE);
        if (ISUNDEF(h))
        {
            client->app->chatlink_result(UNDEF, API_EINTERNAL);
            return false;
        }
        else
        {
            client->app->chatlink_result(h, API_OK);
            return true;
        }
    }
}

CommandChatLinkURL::CommandChatLinkURL(MegaClient *client, handle publichandle)
{
    cmd("mcphurl");
    arg("ph", (byte*)&publichandle, MegaClient::CHATLINKHANDLE);

    notself(client);
    tag = client->reqtag;
}

bool CommandChatLinkURL::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->chatlinkurl_result(UNDEF, -1, NULL, NULL, -1, 0, false, UNDEF, r.errorOrOK());
        return true;
    }
    else
    {
        handle chatid = UNDEF;
        int shard = -1;
        int numPeers = -1;
        string url;
        string ct;
        m_time_t ts = 0;
        bool meetingRoom = false;
        handle callid = UNDEF;

        for (;;)
        {
            switch (client->json.getnameid())
            {
                case MAKENAMEID2('i','d'):
                    chatid = client->json.gethandle(MegaClient::CHATHANDLE);
                    break;

                case MAKENAMEID2('c','s'):
                    shard = int(client->json.getint());
                    break;

                case MAKENAMEID2('c','t'):  // chat-title
                    client->json.storeobject(&ct);
                    break;

                case MAKENAMEID3('u','r','l'):
                    client->json.storeobject(&url);
                    break;

                case MAKENAMEID3('n','c','m'):
                    numPeers = int(client->json.getint());
                    break;

                case MAKENAMEID2('t', 's'):
                    ts = client->json.getint();
                    break;

                case MAKENAMEID6('c', 'a', 'l', 'l', 'I', 'd'):
                    callid = client->json.gethandle(MegaClient::CHATHANDLE);
                    break;

                case MAKENAMEID2('m', 'r'):
                    meetingRoom = client->json.getbool();
                    break;

                case EOO:
                    if (chatid != UNDEF && shard != -1 && !url.empty() && !ct.empty() && numPeers != -1)
                    {
                        client->app->chatlinkurl_result(chatid, shard, &url, &ct, numPeers, ts, meetingRoom, callid, API_OK);
                    }
                    else
                    {
                        client->app->chatlinkurl_result(UNDEF, -1, NULL, NULL, -1, 0, false, UNDEF, API_EINTERNAL);
                    }
                    return true;

                default:
                    if (!client->json.storeobject())
                    {
                        client->app->chatlinkurl_result(UNDEF, -1, NULL, NULL, -1, 0, false, UNDEF, API_EINTERNAL);
                        return false;
                    }
            }
        }
    }
}

CommandChatLinkClose::CommandChatLinkClose(MegaClient *client, handle chatid, const char *title)
{
    mChatid = chatid;
    mTitle = title ? string(title) : "";

    cmd("mcscm");
    arg("id", (byte*)&chatid, MegaClient::CHATHANDLE);

    if (title)
    {
        arg("ct", title);
    }

    notself(client);
    tag = client->reqtag;
}

bool CommandChatLinkClose::procresult(Result r)
{
    if (r.wasError(API_OK))
    {
        textchat_map::iterator it = client->chats.find(mChatid);
        if (it == client->chats.end())
        {
            LOG_err << "Chat link close succeeded for a non-existing chatroom";
            client->app->chatlinkclose_result(API_ENOENT);
            return true;
        }

        TextChat *chat = it->second;
        chat->setMode(false);
        if (!mTitle.empty())
        {
            chat->title = mTitle;
        }

        chat->setTag(tag ? tag : -1);
        client->notifychat(chat);
    }

    client->app->chatlinkclose_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandChatLinkJoin::CommandChatLinkJoin(MegaClient *client, handle publichandle, const char *unifiedkey)
{
    cmd("mciph");
    arg("ph", (byte*)&publichandle, MegaClient::CHATLINKHANDLE);
    arg("ck", unifiedkey);
    tag = client->reqtag;
}

bool CommandChatLinkJoin::procresult(Result r)
{
    client->app->chatlinkjoin_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

#endif

CommandGetMegaAchievements::CommandGetMegaAchievements(MegaClient *client, AchievementsDetails *details, bool registered_user)
{
    this->details = details;

    if (registered_user)
    {
        cmd("maf");
    }
    else
    {
        cmd("mafu");
    }

    arg("v", (m_off_t)0);

    tag = client->reqtag;
}

bool CommandGetMegaAchievements::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getmegaachievements_result(details, r.errorOrOK());
        return true;
    }

    details->permanent_size = 0;
    details->achievements.clear();
    details->awards.clear();
    details->rewards.clear();

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 's':
                details->permanent_size = client->json.getint();
                break;

            case 'u':
                if (client->json.enterobject())
                {
                    for (;;)
                    {
                        achievement_class_id id = achievement_class_id(client->json.getnameid());
                        if (id == EOO)
                        {
                            break;
                        }
                        id -= '0';   // convert to number

                        if (client->json.enterarray())
                        {
                            Achievement achievement;
                            achievement.storage = client->json.getint();
                            achievement.transfer = client->json.getint();
                            const char *exp_ts = client->json.getvalue();
                            char *pEnd = NULL;
                            achievement.expire = int(strtol(exp_ts, &pEnd, 10));
                            if (*pEnd == 'm')
                            {
                                achievement.expire *= 30;
                            }
                            else if (*pEnd == 'y')
                            {
                                achievement.expire *= 365;
                            }

                            details->achievements[id] = achievement;

                            while(client->json.storeobject());
                            client->json.leavearray();
                        }
                    }

                    client->json.leaveobject();
                }
                else
                {
                    LOG_err << "Failed to parse Achievements of MEGA achievements";
                    client->json.storeobject();
                    client->app->getmegaachievements_result(details, API_EINTERNAL);
                    return false;
                }
                break;

            case 'a':
                if (client->json.enterarray())
                {
                    while (client->json.enterobject())
                    {
                        Award award;
                        award.achievement_class = 0;
                        award.award_id = 0;
                        award.ts = 0;
                        award.expire = 0;

                        bool finished = false;
                        while (!finished)
                        {
                            switch (client->json.getnameid())
                            {
                            case 'a':
                                award.achievement_class = achievement_class_id(client->json.getint());
                                break;
                            case 'r':
                                award.award_id = int(client->json.getint());
                                break;
                            case MAKENAMEID2('t', 's'):
                                award.ts = client->json.getint();
                                break;
                            case 'e':
                                award.expire = client->json.getint();
                                break;
                            case 'm':
                                if (client->json.enterarray())
                                {
                                    string email;
                                    while(client->json.storeobject(&email))
                                    {
                                        award.emails_invited.push_back(email);
                                    }

                                    client->json.leavearray();
                                }
                                break;
                            case EOO:
                                finished = true;
                                break;
                            default:
                                client->json.storeobject();
                                break;
                            }
                        }

                        details->awards.push_back(award);

                        client->json.leaveobject();
                    }

                    client->json.leavearray();
                }
                else
                {
                    LOG_err << "Failed to parse Awards of MEGA achievements";
                    client->json.storeobject();
                    client->app->getmegaachievements_result(details, API_EINTERNAL);
                    return false;
                }
                break;

            case 'r':
                if (client->json.enterobject())
                {
                    for (;;)
                    {
                        nameid id = client->json.getnameid();
                        if (id == EOO)
                        {
                            break;
                        }

                        Reward reward;
                        reward.award_id = int(id - '0');   // convert to number

                        client->json.enterarray();

                        reward.storage = client->json.getint();
                        reward.transfer = client->json.getint();
                        const char *exp_ts = client->json.getvalue();
                        char *pEnd = NULL;
                        reward.expire = int(strtol(exp_ts, &pEnd, 10));
                        if (*pEnd == 'm')
                        {
                            reward.expire *= 30;
                        }
                        else if (*pEnd == 'y')
                        {
                            reward.expire *= 365;
                        }

                        while(client->json.storeobject());
                        client->json.leavearray();

                        details->rewards.push_back(reward);
                    }

                    client->json.leaveobject();
                }
                else
                {
                    LOG_err << "Failed to parse Rewards of MEGA achievements";
                    client->json.storeobject();
                    client->app->getmegaachievements_result(details, API_EINTERNAL);
                    return false;
                }
                break;

            case EOO:
                client->app->getmegaachievements_result(details, API_OK);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse MEGA achievements";
                    client->app->getmegaachievements_result(details, API_EINTERNAL);
                    return false;
                }
                break;
        }
    }
}

CommandGetWelcomePDF::CommandGetWelcomePDF(MegaClient *client)
{
    cmd("wpdf");

    tag = client->reqtag;
}

bool CommandGetWelcomePDF::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        LOG_err << "Unexpected response of 'wpdf' command: missing 'ph' and 'k'";
        return true;
    }

    handle ph = UNDEF;
    byte keybuf[FILENODEKEYLENGTH];
    int len_key = 0;
    string key;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID2('p', 'h'):
                ph = client->json.gethandle(MegaClient::NODEHANDLE);
                break;

            case 'k':
                len_key = client->json.storebinary(keybuf, sizeof keybuf);
                break;

            case EOO:
                if (ISUNDEF(ph) || len_key != FILENODEKEYLENGTH)
                {
                    LOG_err << "Failed to import welcome PDF: invalid response";
                    return false;
                }
                key.assign((const char *) keybuf, len_key);
                client->reqs.add(new CommandGetPH(client, ph, (const byte*) key.data(), 2));
                return true;

            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse welcome PDF response";
                    return false;
                }
                break;
        }
    }
}


CommandMediaCodecs::CommandMediaCodecs(MegaClient* c, Callback cb)
{
    cmd("mc");

    client = c;
    callback = cb;
}

bool CommandMediaCodecs::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        LOG_err << "mc result: " << error(r.errorOrOK());
        return true;
    }

    if (!client->json.isnumeric())
    {
        // It's wrongly formatted, consume this one so the next command can be processed.
        LOG_err << "mc response badly formatted";
        return false;
    }

    int version = static_cast<int>(client->json.getint());
    callback(client, version);
    return true;
}

CommandContactLinkCreate::CommandContactLinkCreate(MegaClient *client, bool renew)
{
    if (renew)
    {
        cmd("clr");
    }
    else
    {
        cmd("clc");
    }

    tag = client->reqtag;
}

bool CommandContactLinkCreate::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->contactlinkcreate_result(r.errorOrOK(), UNDEF);
    }
    else
    {
        handle h = client->json.gethandle(MegaClient::CONTACTLINKHANDLE);
        client->app->contactlinkcreate_result(API_OK, h);
    }
    return true;
}

CommandContactLinkQuery::CommandContactLinkQuery(MegaClient *client, handle h)
{
    cmd("clg");
    arg("cl", (byte*)&h, MegaClient::CONTACTLINKHANDLE);

    arg("b", 1);    // return firstname/lastname in B64

    tag = client->reqtag;
}

bool CommandContactLinkQuery::procresult(Result r)
{
    handle h = UNDEF;
    string email;
    string firstname;
    string lastname;
    string avatar;

    if (r.wasErrorOrOK())
    {
        client->app->contactlinkquery_result(r.errorOrOK(), h, &email, &firstname, &lastname, &avatar);
        return true;
    }

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case 'h':
                h = client->json.gethandle(MegaClient::USERHANDLE);
                break;
            case 'e':
                client->json.storeobject(&email);
                break;
            case MAKENAMEID2('f', 'n'):
                client->json.storeobject(&firstname);
                break;
            case MAKENAMEID2('l', 'n'):
                client->json.storeobject(&lastname);
                break;
            case MAKENAMEID2('+', 'a'):
                client->json.storeobject(&avatar);
                break;
            case EOO:
                client->app->contactlinkquery_result(API_OK, h, &email, &firstname, &lastname, &avatar);
                return true;
            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse query contact link response";
                    client->app->contactlinkquery_result(API_EINTERNAL, h, &email, &firstname, &lastname, &avatar);
                    return false;
                }
                break;
        }
    }
}

CommandContactLinkDelete::CommandContactLinkDelete(MegaClient *client, handle h)
{
    cmd("cld");
    if (!ISUNDEF(h))
    {
        arg("cl", (byte*)&h, MegaClient::CONTACTLINKHANDLE);
    }
    tag = client->reqtag;
}

bool CommandContactLinkDelete::procresult(Result r)
{
    client->app->contactlinkdelete_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandKeepMeAlive::CommandKeepMeAlive(MegaClient *client, int type, bool enable)
{
    if (enable)
    {
        cmd("kma");
    }
    else
    {
        cmd("kmac");
    }
    arg("t", type);

    tag = client->reqtag;
}

bool CommandKeepMeAlive::procresult(Result r)
{
    client->app->keepmealive_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandMultiFactorAuthSetup::CommandMultiFactorAuthSetup(MegaClient *client, const char *pin)
{
    cmd("mfas");
    if (pin)
    {
        arg("mfa", pin);
    }
    tag = client->reqtag;
}

bool CommandMultiFactorAuthSetup::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->multifactorauthsetup_result(NULL, r.errorOrOK());
        return true;
    }

    string code;
    if (!client->json.storeobject(&code))
    {
        client->app->multifactorauthsetup_result(NULL, API_EINTERNAL);
        return false;
    }
    client->app->multifactorauthsetup_result(&code, API_OK);
    return true;
}

CommandMultiFactorAuthCheck::CommandMultiFactorAuthCheck(MegaClient *client, const char *email)
{
    cmd("mfag");
    arg("e", email);

    tag = client->reqtag;
}

bool CommandMultiFactorAuthCheck::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->multifactorauthcheck_result(r.errorOrOK());
        return true;
    }

    if (client->json.isnumeric())
    {
        client->app->multifactorauthcheck_result(static_cast<int>(client->json.getint()));
        return true;
    }
    else
    {
        client->app->multifactorauthcheck_result(API_EINTERNAL);
        return false;
    }
}

CommandMultiFactorAuthDisable::CommandMultiFactorAuthDisable(MegaClient *client, const char *pin)
{
    cmd("mfad");
    arg("mfa", pin);

    tag = client->reqtag;
}

bool CommandMultiFactorAuthDisable::procresult(Result r)
{
    client->app->multifactorauthdisable_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandGetPSA::CommandGetPSA(bool urlSupport, MegaClient *client)
{
    cmd("gpsa");

    if (urlSupport)
    {
        arg("w", 1);
    }

    tag = client->reqtag;
}

bool CommandGetPSA::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getpsa_result(r.errorOrOK(), 0, NULL, NULL, NULL, NULL, NULL, NULL);
        return true;
    }

    int id = 0;
    string temp;
    string title, text, imagename, imagepath;
    string buttonlink, buttontext, url;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID2('i', 'd'):
                id = int(client->json.getint());
                break;
            case 't':
                client->json.storeobject(&temp);
                Base64::atob(temp, title);
                break;
            case 'd':
                client->json.storeobject(&temp);
                Base64::atob(temp, text);
                break;
            case MAKENAMEID3('i', 'm', 'g'):
                client->json.storeobject(&imagename);
                break;
            case 'l':
                client->json.storeobject(&buttonlink);
                break;
            case MAKENAMEID3('u', 'r', 'l'):
                client->json.storeobject(&url);
                break;
            case 'b':
                client->json.storeobject(&temp);
                Base64::atob(temp, buttontext);
                break;
            case MAKENAMEID3('d', 's', 'p'):
                client->json.storeobject(&imagepath);
                break;
            case EOO:
                imagepath.append(imagename);
                imagepath.append(".png");
                client->app->getpsa_result(API_OK, id, &title, &text, &imagepath, &buttontext, &buttonlink, &url);
                return true;
            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse get PSA response";
                    client->app->getpsa_result(API_EINTERNAL, 0, NULL, NULL, NULL, NULL, NULL, NULL);
                    return false;
                }
                break;
        }
    }
}

CommandFetchTimeZone::CommandFetchTimeZone(MegaClient *client, const char *timezone, const char* timeoffset)
{
    cmd("ftz");
    arg("utz", timezone);
    arg("uo", timeoffset);

    tag = client->reqtag;
}

bool CommandFetchTimeZone::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->fetchtimezone_result(r.errorOrOK(), NULL, NULL, -1);
        return true;
    }

    string currenttz;
    int currentto;
    vector<string> timezones;
    vector<int> timeoffsets;
    string defaulttz;
    int defaulttzindex = -1;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID7('c', 'h', 'o', 'i', 'c', 'e', 's'):
                if (client->json.enterobject())
                {
                    while (client->json.storeobject(&currenttz))
                    {
                        currentto = int(client->json.getint());
                        timezones.push_back(currenttz);
                        timeoffsets.push_back(currentto);
                    }
                    client->json.leaveobject();
                }
                else if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse fetch time zone response";
                    client->app->fetchtimezone_result(API_EINTERNAL, NULL, NULL, -1);
                    return false;
                }
                break;

            case MAKENAMEID7('d', 'e', 'f', 'a', 'u', 'l', 't'):
                if (client->json.isnumeric())
                {
                    client->json.getint();
                }
                else
                {
                    client->json.storeobject(&defaulttz);
                }
                break;

            case EOO:
                if (!defaulttz.empty())    // default received as string
                {
                    for (int i = 0; i < (int)timezones.size(); i++)
                    {
                        if (timezones[i] == defaulttz)
                        {
                            defaulttzindex = i;
                            break;
                        }
                    }
                }
                client->app->fetchtimezone_result(API_OK, &timezones, &timeoffsets, defaulttzindex);
                return true;

            default:
                if (!client->json.storeobject())
                {
                    LOG_err << "Failed to parse fetch time zone response";
                    client->app->fetchtimezone_result(API_EINTERNAL, NULL, NULL, -1);
                    return false;
                }
                break;
        }
    }
}

CommandSetLastAcknowledged::CommandSetLastAcknowledged(MegaClient* client)
{
    cmd("sla");
    notself(client);
    tag = client->reqtag;
}

bool CommandSetLastAcknowledged::procresult(Result r)
{
    client->app->acknowledgeuseralerts_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandSMSVerificationSend::CommandSMSVerificationSend(MegaClient* client, const string& phoneNumber, bool reVerifyingWhitelisted)
{
    cmd("smss");
    batchSeparately = true;  // don't let any other commands that might get batched with it cause the whole batch to fail

    assert(isPhoneNumber(phoneNumber));
    arg("n", phoneNumber.c_str());

    if (reVerifyingWhitelisted)
    {
        arg("to", 1);   // test override
    }

    tag = client->reqtag;
}

bool CommandSMSVerificationSend::isPhoneNumber(const string& s)
{
    for (auto i = s.size(); i--; )
    {
        if (!(isdigit(s[i]) || (i == 0 && s[i] == '+')))
        {
            return false;
        }
    }
    return s.size() > 6;
}

bool CommandSMSVerificationSend::procresult(Result r)
{
    client->app->smsverificationsend_result(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandSMSVerificationCheck::CommandSMSVerificationCheck(MegaClient* client, const string& verificationcode)
{
    cmd("smsv");
    batchSeparately = true;  // don't let any other commands that might get batched with it cause the whole batch to fail

    if (isVerificationCode(verificationcode))
    {
        arg("c", verificationcode.c_str());
    }

    tag = client->reqtag;
}

bool CommandSMSVerificationCheck::isVerificationCode(const string& s)
{
    for (const char c : s)
    {
        if (!isdigit(c))
        {
            return false;
        }
    }
    return s.size() == 6;
}

bool CommandSMSVerificationCheck::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->smsverificationcheck_result(r.errorOrOK(), nullptr);
        return true;
    }

    string phoneNumber;
    if (!client->json.storeobject(&phoneNumber))
    {
        client->app->smsverificationcheck_result(API_EINTERNAL, nullptr);
        return false;
    }

    assert(CommandSMSVerificationSend::isPhoneNumber(phoneNumber));
    client->mSmsVerifiedPhone = phoneNumber;
    client->app->smsverificationcheck_result(API_OK, &phoneNumber);
    return true;
}

CommandGetRegisteredContacts::CommandGetRegisteredContacts(MegaClient* client, const map<const char*, const char*>& contacts)
{
    cmd("usabd");

    arg("v", 1);

    beginobject("e");
    for (const auto& pair : contacts)
    {
        arg(Base64::btoa(pair.first).c_str(), // name is text-input from user, need conversion too
            (byte *)pair.second, static_cast<int>(strlen(pair.second)));
    }
    endobject();

    tag = client->reqtag;
}

bool CommandGetRegisteredContacts::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getregisteredcontacts_result(r.errorOrOK(), nullptr);
        return true;
    }

    vector<tuple<string, string, string>> registeredContacts;

    string entryUserDetail;
    string id;
    string userDetail;

    bool success = true;
    while (client->json.enterobject())
    {
        bool exit = false;
        while (!exit)
        {
            switch (client->json.getnameid())
            {
                case MAKENAMEID3('e', 'u', 'd'):
                {
                    client->json.storeobject(&entryUserDetail);
                    break;
                }
                case MAKENAMEID2('i', 'd'):
                {
                    client->json.storeobject(&id);
                    break;
                }
                case MAKENAMEID2('u', 'd'):
                {
                    client->json.storeobject(&userDetail);
                    break;
                }
                case EOO:
                {
                    if (entryUserDetail.empty() || id.empty() || userDetail.empty())
                    {
                        LOG_err << "Missing or empty field when parsing 'get registered contacts' response";
                        success = false;
                    }
                    else
                    {
                        registeredContacts.emplace_back(
                                    make_tuple(Base64::atob(entryUserDetail), move(id),
                                               Base64::atob(userDetail)));
                    }
                    exit = true;
                    break;
                }
                default:
                {
                    if (!client->json.storeobject())
                    {
                        LOG_err << "Failed to parse 'get registered contacts' response";
                        client->app->getregisteredcontacts_result(API_EINTERNAL, nullptr);
                        return false;
                    }
                }
            }
        }
        client->json.leaveobject();
    }
    if (success)
    {
        client->app->getregisteredcontacts_result(API_OK, &registeredContacts);
        return true;
    }
    else
    {
        client->app->getregisteredcontacts_result(API_EINTERNAL, nullptr);
        return false;
    }
}

CommandGetCountryCallingCodes::CommandGetCountryCallingCodes(MegaClient* client)
{
    cmd("smslc");

    batchSeparately = true;
    tag = client->reqtag;
}

bool CommandGetCountryCallingCodes::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getcountrycallingcodes_result(r.errorOrOK(), nullptr);
        return true;
    }

    map<string, vector<string>> countryCallingCodes;

    bool success = true;
    while (client->json.enterobject())
    {
        bool exit = false;
        string countryCode;
        vector<string> callingCodes;
        while (!exit)
        {
            switch (client->json.getnameid())
            {
                case MAKENAMEID2('c', 'c'):
                {
                    client->json.storeobject(&countryCode);
                    break;
                }
                case MAKENAMEID1('l'):
                {
                    if (client->json.enterarray())
                    {
                        std::string code;
                        while (client->json.storeobject(&code))
                        {
                            callingCodes.emplace_back(move(code));
                        }
                        client->json.leavearray();
                    }
                    break;
                }
                case EOO:
                {
                    if (countryCode.empty() || callingCodes.empty())
                    {
                        LOG_err << "Missing or empty fields when parsing 'get country calling codes' response";
                        success = false;
                    }
                    else
                    {
                        countryCallingCodes.emplace(make_pair(move(countryCode), move(callingCodes)));
                    }
                    exit = true;
                    break;
                }
                default:
                {
                    if (!client->json.storeobject())
                    {
                        LOG_err << "Failed to parse 'get country calling codes' response";
                        client->app->getcountrycallingcodes_result(API_EINTERNAL, nullptr);
                        return false;
                    }
                }
            }
        }
        client->json.leaveobject();
    }
    if (success)
    {
        client->app->getcountrycallingcodes_result(API_OK, &countryCallingCodes);
        return true;
    }
    else
    {
        client->app->getcountrycallingcodes_result(API_EINTERNAL, nullptr);
        return false;
    }
}

CommandFolderLinkInfo::CommandFolderLinkInfo(MegaClient* client, handle publichandle)
{
    ph = publichandle;

    cmd("pli");
    arg("ph", (byte*)&publichandle, MegaClient::NODEHANDLE);

    tag = client->reqtag;
}

bool CommandFolderLinkInfo::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->folderlinkinfo_result(r.errorOrOK(), UNDEF, UNDEF, NULL, NULL, 0, 0, 0, 0, 0);
        return true;
    }
    string attr;
    string key;
    handle owner = UNDEF;
    handle ph = 0;
    m_off_t currentSize = 0;
    m_off_t versionsSize  = 0;
    int numFolders = 0;
    int numFiles = 0;
    int numVersions = 0;

    for (;;)
    {
        switch (client->json.getnameid())
        {
        case MAKENAMEID5('a','t','t','r','s'):
            client->json.storeobject(&attr);
            break;

        case MAKENAMEID2('p','h'):
            ph = client->json.gethandle(MegaClient::NODEHANDLE);
            break;

        case 'u':
            owner = client->json.gethandle(MegaClient::USERHANDLE);
            break;

        case 's':
            if (client->json.enterarray())
            {
                currentSize = client->json.getint();
                numFiles = int(client->json.getint());
                numFolders = int(client->json.getint());
                versionsSize  = client->json.getint();
                numVersions = int(client->json.getint());
                client->json.leavearray();
            }
            break;

        case 'k':
            client->json.storeobject(&key);
            break;

        case EOO:
            if (attr.empty())
            {
                LOG_err << "The folder link information doesn't contain the attr string";
                client->app->folderlinkinfo_result(API_EINCOMPLETE, UNDEF, UNDEF, NULL, NULL, 0, 0, 0, 0, 0);
                return false;
            }
            if (key.size() <= 9 || key.find(":") == string::npos)
            {
                LOG_err << "The folder link information doesn't contain a valid decryption key";
                client->app->folderlinkinfo_result(API_EKEY, UNDEF, UNDEF, NULL, NULL, 0, 0, 0, 0, 0);
                return false;
            }
            if (ph != this->ph)
            {
                LOG_err << "Folder link information: public handle doesn't match";
                client->app->folderlinkinfo_result(API_EINTERNAL, UNDEF, UNDEF, NULL, NULL, 0, 0, 0, 0, 0);
                return false;
            }

            client->app->folderlinkinfo_result(API_OK, owner, ph, &attr, &key, currentSize, numFiles, numFolders, versionsSize, numVersions);
            return true;

        default:
            if (!client->json.storeobject())
            {
                LOG_err << "Failed to parse folder link information response";
                client->app->folderlinkinfo_result(API_EINTERNAL, UNDEF, UNDEF, NULL, NULL, 0, 0, 0, 0, 0);
                return false;
            }
            break;
        }
    }
}

CommandBackupPut::CommandBackupPut(MegaClient* client, const BackupInfo& fields, std::function<void(Error, handle /*backup id*/)> completion)
    : mCompletion(completion)
{
    cmd("sp");

    if (!ISUNDEF(fields.backupId))
    {
        arg("id", (byte*)&fields.backupId, MegaClient::BACKUPHANDLE);
    }

    if (fields.type != BackupType::INVALID)
    {
        arg("t", fields.type);
    }

    if (!fields.nodeHandle.isUndef())
    {
        arg("h", fields.nodeHandle);
    }

    if (!fields.localFolder.empty())
    {
        string localFolderEncrypted(client->cypherTLVTextWithMasterKey("lf", fields.localFolder.toPath(false)));
        arg("l", localFolderEncrypted.c_str());
    }

    if (!fields.deviceId.empty())
    {
        arg("d", fields.deviceId.c_str());
    }

    if (!ISUNDEF(fields.driveId))
    {
        arg("dr",  (byte*)&fields.driveId, MegaClient::DRIVEHANDLE);
    }

    if (fields.state >= 0)
    {
        arg("s", fields.state);
    }

    if (fields.subState >= 0)
    {
        arg("ss", fields.subState);
    }

    if (!fields.backupName.empty())
    {
        string edEncrypted(client->cypherTLVTextWithMasterKey("bn", fields.backupName));
        arg("e", edEncrypted.c_str());
    }

    tag = client->reqtag;
}

bool CommandBackupPut::procresult(Result r)
{
    assert(r.wasStrictlyError() || r.hasJsonItem());
    handle backupId = UNDEF;
    Error e = API_OK;

    if (r.hasJsonItem())
    {
        backupId = client->json.gethandle(MegaClient::BACKUPHANDLE);
        e = API_OK;
    }
    else
    {
        e = r.errorOrOK();
    }

    assert(e != API_EARGS);  // if this happens, the API rejected the request because it wants more fields supplied

    if (mCompletion) mCompletion(e, backupId);

    client->app->backupput_result(e, backupId);

    return r.wasStrictlyError() || r.hasJsonItem();
}

CommandBackupPutHeartBeat::CommandBackupPutHeartBeat(MegaClient* client, handle backupId, SPHBStatus status, int8_t progress, uint32_t uploads, uint32_t downloads, m_time_t ts, handle lastNode, std::function<void(Error)> f)
    : mCompletion(f)
{
    cmd("sphb");

    arg("id", (byte*)&backupId, MegaClient::BACKUPHANDLE);
    arg("s", uint8_t(status));
    if (status == SPHBStatus::SYNCING || status == SPHBStatus::UPTODATE)
    {
        // so don't send 0 out of 0 0% initially
        assert(progress >= 0);
        assert(progress <= 100);
        arg("p", progress);
    }
    arg("qu", uploads);
    arg("qd", downloads);
    if (ts != -1)
    {
        arg("lts", ts);
    }
    if (!ISUNDEF(lastNode))
    {
        arg("lh", (byte*)&lastNode, MegaClient::NODEHANDLE);
    }

    tag = client->reqtag;
}

bool CommandBackupPutHeartBeat::procresult(Result r)
{
    if (mCompletion) mCompletion(r.errorOrOK());
    return r.wasErrorOrOK();
}

CommandBackupRemove::CommandBackupRemove(MegaClient *client, handle backupId, std::function<void(Error)> completion)
    : mBackupId(backupId)
{
    cmd("sr");
    arg("id", (byte*)&backupId, MegaClient::BACKUPHANDLE);

    tag = client->reqtag;
    mCompletion = completion;
}

bool CommandBackupRemove::procresult(Result r)
{
    if (mCompletion)
    {
        mCompletion(r.errorOrOK());
    }
    return r.wasErrorOrOK();
}

CommandBackupSyncFetch::CommandBackupSyncFetch(std::function<void(Error, vector<Data>&)> f)
    : completion(move(f))
{
    cmd("sf");
}

bool CommandBackupSyncFetch::procresult(Result r)
{
    vector<Data> data;
    if (!r.hasJsonArray())
    {
        completion(r.errorOrOK(), data);
    }
    else
    {
        auto skipUnknownField = [&]() -> bool {
            if (!client->json.storeobject())
            {
                completion(API_EINTERNAL, data);
                return false;
            }
            return true;
        };

        auto cantLeaveObject = [&]() -> bool {
            if (!client->json.leaveobject())
            {
                completion(API_EINTERNAL, data);
                return true;
            }
            return false;
        };

        while (client->json.enterobject())
        {
            data.push_back(Data());
            for (;;)
            {
                auto& d = data.back();
                auto nid = client->json.getnameid();
                if (nid == EOO) break;
                switch (nid)
                {
                case MAKENAMEID2('i', 'd'):     d.backupId = client->json.gethandle(sizeof(handle)); break;
                case MAKENAMEID1('t'):          d.backupType = static_cast<BackupType>(client->json.getint32()); break;
                case MAKENAMEID1('h'):          d.rootNode = client->json.gethandle(MegaClient::NODEHANDLE); break;
                case MAKENAMEID1('l'):          client->json.storeobject(&d.localFolder);
                                                d.localFolder = client->decypherTLVTextWithMasterKey("lf", d.localFolder);
                                                break;
                case MAKENAMEID1('d'):          client->json.storeobject(&d.deviceId); break;
                case MAKENAMEID1('s'):          d.syncState = client->json.getint32(); break;
                case MAKENAMEID2('s', 's'):     d.syncSubstate = client->json.getint32(); break;
                case MAKENAMEID1('e'):          client->json.storeobject(&d.extra);
                                                d.backupName = client->decypherTLVTextWithMasterKey("bn", d.extra);
                                                break;
                case MAKENAMEID2('h', 'b'):
                {

                    if (client->json.enterobject())
                    {
                        for (;;)
                        {
                            nid = client->json.getnameid();
                            if (nid == EOO) break;
                            switch (nid)
                            {
                            case MAKENAMEID2('t', 's'):     d.hbTimestamp = client->json.getint(); break;
                            case MAKENAMEID1('s'):          d.hbStatus = client->json.getint32(); break;
                            case MAKENAMEID1('p'):          d.hbProgress = client->json.getint32(); break;
                            case MAKENAMEID2('q', 'u'):     d.uploads = client->json.getint32(); break;
                            case MAKENAMEID2('q', 'd'):     d.downloads = client->json.getint32(); break;
                            case MAKENAMEID3('l', 't', 's'):d.lastActivityTs = client->json.getint32(); break;
                            case MAKENAMEID2('l', 'h'):     d.lastSyncedNodeHandle = client->json.gethandle(MegaClient::NODEHANDLE); break;
                            default: if (!skipUnknownField()) return false;
                            }
                        }
                        if (cantLeaveObject()) return false;
                    }
                }
                break;

                default: if (!skipUnknownField()) return false;
                }
            }
            if (cantLeaveObject()) return false;
        }

        completion(API_OK, data);
    }
    return true;
}


CommandGetBanners::CommandGetBanners(MegaClient* client)
{
    cmd("gban");

    tag = client->reqtag;
}

bool CommandGetBanners::procresult(Result r)
{
    if (r.wasErrorOrOK())
    {
        client->app->getbanners_result(r.errorOrOK());
        return true; // because parsing didn't fail
    }

    /*
        {
            "id": 2, ///The banner id
            "t": "R2V0IFZlcmlmaWVk", ///Banner title
            "d": "TWFrZSBpdCBlYXNpZXIgZm9yIHlvdXIgY29udGFjdHMgdG8gZmluZCB5b3Ugb24gTUVHQS4", ///Banner description.
            "img": "Verified_image.png", ///Image name.
            "l": "", ///URL
            "bimg": "Verified_BG.png", ///background image name.
            "dsp": "https://domain/path" ///Where to get the image.
        }, {"id":3, ...}, ... ]
    */

    vector< tuple<int, string, string, string, string, string, string> > banners;

    // loop array elements
    while (client->json.enterobject())
    {
        int id = 0;
        string title, description, img, url, bimg, dsp;
        bool exit = false;

        // loop and read object members
        while (!exit)
        {
            switch (client->json.getnameid())
            {
            case MAKENAMEID2('i', 'd'):
                id = client->json.getint32();
                break;

            case MAKENAMEID1('t'):
                client->json.storeobject(&title);
                title = Base64::atob(title);
                break;

            case MAKENAMEID1('d'):
                client->json.storeobject(&description);
                description = Base64::atob(description);
                break;

            case MAKENAMEID3('i', 'm', 'g'):
                client->json.storeobject(&img);
                break;

            case MAKENAMEID1('l'):
                client->json.storeobject(&url);
                break;

            case MAKENAMEID4('b', 'i', 'm', 'g'):
                client->json.storeobject(&bimg);
                break;

            case MAKENAMEID3('d', 's', 'p'):
                client->json.storeobject(&dsp);
                break;

            case EOO:
                if (!id || title.empty() || description.empty())
                {
                    LOG_err << "Missing id, title or description in response to gban";
                    client->app->getbanners_result(API_EINTERNAL);
                    return false;
                }
                exit = true;
                break;

            default:
                if (!client->json.storeobject()) // skip unknown member
                {
                    LOG_err << "Failed to parse banners response";
                    client->app->getbanners_result(API_EINTERNAL);
                    return false;
                }
                break;
            }
        }

        banners.emplace_back(make_tuple(id, move(title), move(description), move(img), move(url), move(bimg), move(dsp)));

        client->json.leaveobject();
    }

    client->app->getbanners_result(move(banners));

    return true;
}

CommandDismissBanner::CommandDismissBanner(MegaClient* client, int id, m_time_t timestamp)
{
    cmd("dban");
    arg("id", id); // id of the Smart Banner
    arg("ts", timestamp);

    tag = client->reqtag;
}

bool CommandDismissBanner::procresult(Result r)
{
    client->app->dismissbanner_result(r.errorOrOK());
    return r.wasErrorOrOK();
}


//
// Sets and Elements
//

bool CommandSE::procresultid(const Result& r, handle& id, m_time_t& ts, handle* u, handle* s, int64_t* o) const
{
    if (r.hasJsonObject())
    {
        for (;;)
        {
            switch (client->json.getnameid())
            {
            case MAKENAMEID2('i', 'd'):
                id = client->json.gethandle(MegaClient::SETHANDLE);
                break;

            case MAKENAMEID1('u'):
                if (u)
                {
                    *u = client->json.gethandle(MegaClient::USERHANDLE);
                }
                else if(!client->json.storeobject())
                {
                    return false;
                }
                break;

            case MAKENAMEID1('s'):
                if (s)
                {
                    *s = client->json.gethandle(MegaClient::SETHANDLE);
                }
                else if(!client->json.storeobject())
                {
                    return false;
                }
                break;

            case MAKENAMEID2('t', 's'):
                ts = client->json.getint();
                break;

            case MAKENAMEID1('o'):
                if (o)
                {
                    *o = client->json.getint();
                }
                else if (!client->json.storeobject())
                {
                    return false;
                }
                break;

            default:
                if (!client->json.storeobject())
                {
                    return false;
                }
                break;

            case EOO:
                return true;
            }
        }
    }

    return false;
}

bool CommandSE::procerrorcode(const Result& r, Error& e) const
{
    if (r.wasErrorOrOK())
    {
        e = r.errorOrOK();
        return true;
    }

    return false;
}

CommandPutSet::CommandPutSet(MegaClient* cl, Set&& s, unique_ptr<string> encrAttrs, string&& encrKey,
                             std::function<void(Error, const Set*)> completion)
    : mSet(new Set(move(s))), mCompletion(completion)
{
    cmd("asp");

    if (mSet->id() == UNDEF) // create new
    {
        arg("k", (byte*)encrKey.c_str(), (int)encrKey.size());
    }
    else // update
    {
        arg("id", (byte*)&mSet->id(), MegaClient::SETHANDLE);
    }

    if (encrAttrs)
    {
        arg("at", (byte*)encrAttrs->c_str(), (int)encrAttrs->size());
    }

    notself(cl); // don't process its Action Packet after sending this
}

bool CommandPutSet::procresult(Result r)
{
    handle sId = 0;
    handle user = 0;
    m_time_t ts = 0;
    const Set* s = nullptr;
    Error e = API_OK;
    bool parsedOk = procerrorcode(r, e) || procresultid(r, sId, ts, &user);

    if (!parsedOk || (mSet->id() == UNDEF && !user))
    {
        e = API_EINTERNAL;
    }
    else if (e == API_OK)
    {
        mSet->setTs(ts);
        if (mSet->id() == UNDEF) // add new
        {
            mSet->setId(sId);
            mSet->setUser(user);
            mSet->setChanged(Set::CH_NEW);
            s = client->addSet(move(*mSet));
        }
        else // update existing
        {            
            assert(mSet->id() == sId);

            if (!client->updateSet(move(*mSet)))
            {
                LOG_warn << "Sets: command 'asp' succeed, but Set was not found";
                e = API_ENOENT;
            }
        }
    }

    if (mCompletion)
    {
        mCompletion(e, s);
    }

    return parsedOk;
}

CommandRemoveSet::CommandRemoveSet(MegaClient* cl, handle id, std::function<void(Error)> completion)
    : mSetId(id), mCompletion(completion)
{
    cmd("asr");
    arg("id", (byte*)&id, MegaClient::SETHANDLE);

    notself(cl); // don't process its Action Packet after sending this
}

bool CommandRemoveSet::procresult(Result r)
{
    Error e = API_OK;
    bool parsedOk = procerrorcode(r, e);

    if (parsedOk && e == API_OK)
    {
        if (!client->deleteSet(mSetId))
        {
            LOG_err << "Sets: Failed to remove Set in `asr` command response";
            e = API_ENOENT;
        }
    }

    if (mCompletion)
    {
        mCompletion(e);
    }

    return parsedOk;
}

CommandFetchSet::CommandFetchSet(MegaClient*, handle id,
    std::function<void(Error, Set*, map<handle, SetElement>*)> completion)
    : mCompletion(completion)
{
    cmd("aft");
    arg("id", (byte*)&id, MegaClient::SETHANDLE);
}

bool CommandFetchSet::procresult(Result r)
{
    Error e = API_OK;
    if (procerrorcode(r, e))
    {
        if (mCompletion)
        {
            mCompletion(e, nullptr, nullptr);
        }
        return true;
    }

    map<handle, Set> sets;
    map<handle, map<handle, SetElement>> elements;
    e = client->readSetsAndElements(client->json, sets, elements);
    if (e != API_OK)
    {
        LOG_err << "Sets: Failed to parse \"aft\" response";
        if (mCompletion)
        {
            mCompletion(e, nullptr, nullptr);
        }
        return false;
    }

    assert(sets.size() <= 1);

    if (mCompletion)
    {
        Set* s = sets.empty() ? new Set() : (new Set(move(sets.begin()->second)));
        map<handle, SetElement>* els = elements.empty()
                ? new map<handle, SetElement>()
            : new map<handle, SetElement>(move(elements.begin()->second));
        mCompletion(API_OK, s, els);
    }

    return true;
}

CommandPutSetElement::CommandPutSetElement(MegaClient* cl, SetElement&& el, unique_ptr<string> encrAttrs, string&& encrKey,
                                               std::function<void(Error, const SetElement*)> completion)
    : mElement(new SetElement(move(el))), mCompletion(completion)
{
    cmd("aep");

    bool createNew = mElement->id() == UNDEF;

    if (createNew)
    {
        arg("s", (byte*)&mElement->set(), MegaClient::SETHANDLE);
        arg("h", (byte*)&mElement->node(), MegaClient::NODEHANDLE);
        arg("k", (byte*)encrKey.c_str(), (int)encrKey.size());
    }

    else // update
    {
        arg("id", (byte*)&mElement->id(), MegaClient::SETELEMENTHANDLE);
    }

    // optionals
    if (mElement->hasOrder())
    {
        arg("o", mElement->order());
    }

    if (encrAttrs)
    {
        arg("at", (byte*)encrAttrs->c_str(), (int)encrAttrs->size());
    }

    notself(cl); // don't process its Action Packet after sending this
}

bool CommandPutSetElement::procresult(Result r)
{
    handle elementId = 0;
    m_time_t ts = 0;
    int64_t order = 0;
    Error e = API_OK;
#ifdef DEBUG
    bool isNew = mElement->id() == UNDEF;
#endif
    const SetElement* el = nullptr;
    bool parsedOk = procerrorcode(r, e) || procresultid(r, elementId, ts, nullptr, nullptr, &order); // 'aep' does not return 's'

    if (!parsedOk)
    {
        e = API_EINTERNAL;
    }
    else if (e == API_OK)
    {
        mElement->setTs(ts);
        mElement->setOrder(order); // this is now present in all 'aep' responses
        assert(isNew || mElement->id() == elementId);
        mElement->setId(elementId);
        el = client->addOrUpdateSetElement(move(*mElement));
    }

    if (mCompletion)
    {
        mCompletion(e, el);
    }

    return parsedOk;
}

CommandRemoveSetElement::CommandRemoveSetElement(MegaClient* cl, handle sid, handle eid, std::function<void(Error)> completion)
    : mSetId(sid), mElementId(eid), mCompletion(completion)
{
    cmd("aer");
    arg("id", (byte*)&eid, MegaClient::SETELEMENTHANDLE);

    notself(cl); // don't process its Action Packet after sending this
}

bool CommandRemoveSetElement::procresult(Result r)
{
    handle elementId = 0;
    m_time_t ts = 0;
    Error e = API_OK;
    bool parsedOk = procerrorcode(r, e) || procresultid(r, elementId, ts, nullptr);

    if (parsedOk && e == API_OK)
    {
        if (!client->deleteSetElement(mSetId, mElementId))
        {
            LOG_err << "Sets: Failed to remove Element in `aer` command response";
            e = API_ENOENT;
        }
    }

    if (mCompletion)
    {
        mCompletion(e);
    }

    return parsedOk;
}

// -------- end of Sets and Elements


#ifdef ENABLE_CHAT

bool CommandMeetingStart::procresult(Command::Result r)
{
    if (r.wasErrorOrOK())
    {
        mCompletion(r.errorOrOK(), "", UNDEF);
        return true;
    }

    handle callid = UNDEF;
    string sfuUrl;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID6('c', 'a', 'l', 'l', 'I', 'd'):
                callid = client->json.gethandle(MegaClient::CHATHANDLE);
                break;

            case MAKENAMEID3('s', 'f', 'u'):
                client->json.storeobject(&sfuUrl);
                break;

            case EOO:
                mCompletion(API_OK, sfuUrl, callid);
                return true;
                break;

            default:
                if (!client->json.storeobject())
                {
                    mCompletion(API_EINTERNAL, "", UNDEF);
                    return false;
                }
        }
    }
}

CommandMeetingStart::CommandMeetingStart(MegaClient *client, handle chatid, CommandMeetingStartCompletion completion)
    : mCompletion(completion)
{
    cmd("mcms");
    arg("cid", (byte*)&chatid, MegaClient::CHATHANDLE);
    tag = client->reqtag;
}

bool CommandMeetingJoin::procresult(Command::Result r)
{
    if (r.wasErrorOrOK())
    {
        mCompletion(r.errorOrOK(), "");
        return true;
    }

    string sfuUrl;

    for (;;)
    {
        switch (client->json.getnameid())
        {
            case MAKENAMEID3('u', 'r', 'l'):
                client->json.storeobject(&sfuUrl);
                break;

            case EOO:
                mCompletion(API_OK, sfuUrl);
                return true;
                break;

            default:
                if (!client->json.storeobject())
                {
                    mCompletion(API_EINTERNAL, "");
                    return false;
                }
        }
    }
}

CommandMeetingJoin::CommandMeetingJoin(MegaClient *client, handle chatid, handle callid, CommandMeetingJoinCompletion completion)
    : mCompletion(completion)
{
    cmd("mcmj");
    arg("cid", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("mid", (byte*)&callid, MegaClient::CHATHANDLE);

    tag = client->reqtag;
}

bool CommandMeetingEnd::procresult(Command::Result r)
{
    if (r.wasErrorOrOK())
    {
        mCompletion(r.errorOrOK());
        return true;
    }

    mCompletion(API_EINTERNAL);
    return false;
}

CommandMeetingEnd::CommandMeetingEnd(MegaClient *client, handle chatid, handle callid, int reason, CommandMeetingEndCompletion completion)
    : mCompletion(completion)
{
    cmd("mcme");
    arg("cid", (byte*)&chatid, MegaClient::CHATHANDLE);
    arg("mid", (byte*)&callid, MegaClient::CHATHANDLE);
    // At meeting first version, only valid reason is 0x02 (REJECTED)
    arg("r", reason);

    tag = client->reqtag;
}
#endif

} // namespace
