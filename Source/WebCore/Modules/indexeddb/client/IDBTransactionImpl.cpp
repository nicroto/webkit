/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IDBTransactionImpl.h"

#if ENABLE(INDEXED_DATABASE)

#include "DOMError.h"
#include "EventQueue.h"
#include "IDBDatabaseImpl.h"
#include "IDBError.h"
#include "IDBEventDispatcher.h"
#include "IDBKeyRangeData.h"
#include "IDBObjectStore.h"
#include "IDBRequestImpl.h"
#include "IDBResultData.h"
#include "JSDOMWindowBase.h"
#include "Logging.h"
#include "ScriptExecutionContext.h"
#include "TransactionOperation.h"

namespace WebCore {
namespace IDBClient {

Ref<IDBTransaction> IDBTransaction::create(IDBDatabase& database, const IDBTransactionInfo& info)
{
    return adoptRef(*new IDBTransaction(database, info));
}

IDBTransaction::IDBTransaction(IDBDatabase& database, const IDBTransactionInfo& info)
    : WebCore::IDBTransaction(database.scriptExecutionContext())
    , m_database(database)
    , m_info(info)
    , m_operationTimer(*this, &IDBTransaction::operationTimerFired)

{
    relaxAdoptionRequirement();

    if (m_info.mode() == IndexedDB::TransactionMode::VersionChange) {
        m_originalDatabaseInfo = std::make_unique<IDBDatabaseInfo>(m_database->info());
        m_startedOnServer = true;
    } else {
        activate();

        RefPtr<IDBTransaction> self;
        JSC::VM& vm = JSDOMWindowBase::commonVM();
        vm.whenIdle([self, this]() {
            deactivate();
        });

        establishOnServer();
    }

    suspendIfNeeded();
}

IDBTransaction::~IDBTransaction()
{
}

const String& IDBTransaction::mode() const
{
    switch (m_info.mode()) {
    case IndexedDB::TransactionMode::ReadOnly:
        return IDBTransaction::modeReadOnly();
    case IndexedDB::TransactionMode::ReadWrite:
        return IDBTransaction::modeReadWrite();
    case IndexedDB::TransactionMode::VersionChange:
        return IDBTransaction::modeVersionChange();
    }

    RELEASE_ASSERT_NOT_REACHED();
}

WebCore::IDBDatabase* IDBTransaction::db()
{
    return &m_database.get();
}

IDBConnectionToServer& IDBTransaction::serverConnection()
{
    return m_database->serverConnection();
}

RefPtr<DOMError> IDBTransaction::error() const
{
    ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<WebCore::IDBObjectStore> IDBTransaction::objectStore(const String& objectStoreName, ExceptionCode& ec)
{
    LOG(IndexedDB, "IDBTransaction::objectStore");

    if (objectStoreName.isEmpty()) {
        ec = NOT_FOUND_ERR;
        return nullptr;
    }

    if (isFinishedOrFinishing()) {
        ec = INVALID_STATE_ERR;
        return nullptr;
    }

    auto iterator = m_referencedObjectStores.find(objectStoreName);
    if (iterator != m_referencedObjectStores.end())
        return iterator->value;

    bool found = false;
    for (auto& objectStore : m_info.objectStores()) {
        if (objectStore == objectStoreName) {
            found = true;
            break;
        }
    }

    auto* info = m_database->info().infoForExistingObjectStore(objectStoreName);
    if (!info) {
        ec = NOT_FOUND_ERR;
        return nullptr;
    }

    // Version change transactions are scoped to every object store in the database.
    if (!found && !isVersionChange()) {
        ec = NOT_FOUND_ERR;
        return nullptr;
    }

    auto objectStore = IDBObjectStore::create(*info, *this);
    m_referencedObjectStores.set(objectStoreName, &objectStore.get());

    return adoptRef(&objectStore.leakRef());
}

void IDBTransaction::abort(ExceptionCode& ec)
{
    LOG(IndexedDB, "IDBTransaction::abort");

    if (isFinishedOrFinishing()) {
        ec = INVALID_STATE_ERR;
        return;
    }

    m_state = IndexedDB::TransactionState::Aborting;
    m_database->willAbortTransaction(*this);

    auto operation = createTransactionOperation(*this, nullptr, &IDBTransaction::abortOnServer);
    scheduleOperation(WTF::move(operation));
}

void IDBTransaction::abortOnServer(TransactionOperation&)
{
    LOG(IndexedDB, "IDBTransaction::abortOnServer");
    serverConnection().abortTransaction(*this);
}

const char* IDBTransaction::activeDOMObjectName() const
{
    return "IDBTransaction";
}

bool IDBTransaction::canSuspendForPageCache() const
{
    return false;
}

bool IDBTransaction::hasPendingActivity() const
{
    if (m_state == IndexedDB::TransactionState::Inactive)
        return !m_transactionOperationQueue.isEmpty() || !m_transactionOperationMap.isEmpty();

    return m_state != IndexedDB::TransactionState::Finished;
}

bool IDBTransaction::isActive() const
{
    return m_state == IndexedDB::TransactionState::Active;
}

bool IDBTransaction::isFinishedOrFinishing() const
{
    return m_state == IndexedDB::TransactionState::Committing
        || m_state == IndexedDB::TransactionState::Aborting
        || m_state == IndexedDB::TransactionState::Finished;
}

void IDBTransaction::addRequest(IDBRequest& request)
{
    ASSERT(!m_openRequests.contains(&request));
    m_openRequests.add(&request);
}

void IDBTransaction::removeRequest(IDBRequest& request)
{
    ASSERT(m_openRequests.contains(&request));
    m_openRequests.remove(&request);
}

void IDBTransaction::scheduleOperation(RefPtr<TransactionOperation>&& operation)
{
    ASSERT(!m_transactionOperationMap.contains(operation->identifier()));

    m_transactionOperationQueue.append(operation);
    m_transactionOperationMap.set(operation->identifier(), WTF::move(operation));

    scheduleOperationTimer();
}

void IDBTransaction::scheduleOperationTimer()
{
    if (!m_operationTimer.isActive())
        m_operationTimer.startOneShot(0);
}

void IDBTransaction::operationTimerFired()
{
    LOG(IndexedDB, "IDBTransaction::operationTimerFired");

    if (!m_startedOnServer)
        return;

    if (!m_transactionOperationQueue.isEmpty()) {
        auto operation = m_transactionOperationQueue.takeFirst();
        operation->perform();

        return;
    }

    if (!m_transactionOperationMap.isEmpty() || !m_openRequests.isEmpty())
        return;

    if (!isFinishedOrFinishing())
        commit();
}

void IDBTransaction::commit()
{
    LOG(IndexedDB, "IDBTransaction::commit");

    ASSERT(!isFinishedOrFinishing());

    m_state = IndexedDB::TransactionState::Committing;
    m_database->willCommitTransaction(*this);

    auto operation = createTransactionOperation(*this, nullptr, &IDBTransaction::commitOnServer);
    scheduleOperation(WTF::move(operation));
}

void IDBTransaction::commitOnServer(TransactionOperation&)
{
    LOG(IndexedDB, "IDBTransaction::commitOnServer");
    serverConnection().commitTransaction(*this);
}

void IDBTransaction::finishAbortOrCommit()
{
    ASSERT(m_state != IndexedDB::TransactionState::Finished);
    m_state = IndexedDB::TransactionState::Finished;

    m_originalDatabaseInfo = nullptr;
}

void IDBTransaction::didStart(const IDBError& error)
{
    LOG(IndexedDB, "IDBTransaction::didStart");

    m_database->didStartTransaction(*this);

    m_startedOnServer = true;

    // It's possible the transaction failed to start on the server.
    // That equates to an abort.
    if (!error.isNull()) {
        didAbort(error);
        return;
    }

    scheduleOperationTimer();
}

void IDBTransaction::didAbort(const IDBError& error)
{
    LOG(IndexedDB, "IDBTransaction::didAbort");

    if (m_state == IndexedDB::TransactionState::Finished)
        return;

    m_database->didAbortTransaction(*this);

    m_idbError = error;
    fireOnAbort();

    finishAbortOrCommit();
}

void IDBTransaction::didCommit(const IDBError& error)
{
    LOG(IndexedDB, "IDBTransaction::didCommit");

    ASSERT(m_state == IndexedDB::TransactionState::Committing);

    if (error.isNull()) {
        m_database->didCommitTransaction(*this);
        fireOnComplete();
    } else {
        m_database->didAbortTransaction(*this);
        m_idbError = error;
        fireOnAbort();
    }

    finishAbortOrCommit();
}

void IDBTransaction::fireOnComplete()
{
    LOG(IndexedDB, "IDBTransaction::fireOnComplete");
    enqueueEvent(Event::create(eventNames().completeEvent, false, false));
}

void IDBTransaction::fireOnAbort()
{
    LOG(IndexedDB, "IDBTransaction::fireOnAbort");
    enqueueEvent(Event::create(eventNames().abortEvent, true, false));
}

void IDBTransaction::enqueueEvent(Ref<Event>&& event)
{
    ASSERT(m_state != IndexedDB::TransactionState::Finished);

    if (!scriptExecutionContext())
        return;

    event->setTarget(this);
    scriptExecutionContext()->eventQueue().enqueueEvent(WTF::move(event));
}

bool IDBTransaction::dispatchEvent(Event& event)
{
    LOG(IndexedDB, "IDBTransaction::dispatchEvent");

    ASSERT(scriptExecutionContext());
    ASSERT(event.target() == this);
    ASSERT(event.type() == eventNames().completeEvent || event.type() == eventNames().abortEvent);

    Vector<RefPtr<EventTarget>> targets;
    targets.append(this);
    targets.append(db());

    return IDBEventDispatcher::dispatch(event, targets);
}

Ref<IDBObjectStore> IDBTransaction::createObjectStore(const IDBObjectStoreInfo& info)
{
    LOG(IndexedDB, "IDBTransaction::createObjectStore");
    ASSERT(isVersionChange());

    Ref<IDBObjectStore> objectStore = IDBObjectStore::create(info, *this);
    m_referencedObjectStores.set(info.name(), &objectStore.get());

    auto operation = createTransactionOperation(*this, &IDBTransaction::didCreateObjectStoreOnServer, &IDBTransaction::createObjectStoreOnServer, info);
    scheduleOperation(WTF::move(operation));

    return WTF::move(objectStore);
}

void IDBTransaction::createObjectStoreOnServer(TransactionOperation& operation, const IDBObjectStoreInfo& info)
{
    LOG(IndexedDB, "IDBTransaction::createObjectStoreOnServer");

    ASSERT(isVersionChange());

    m_database->serverConnection().createObjectStore(operation, info);
}

void IDBTransaction::didCreateObjectStoreOnServer(const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didCreateObjectStoreOnServer");

    ASSERT_UNUSED(resultData, resultData.type() == IDBResultType::CreateObjectStoreSuccess);
}

Ref<IDBIndex> IDBTransaction::createIndex(IDBObjectStore& objectStore, const IDBIndexInfo& info)
{
    LOG(IndexedDB, "IDBTransaction::createIndex");
    ASSERT(isVersionChange());

    Ref<IDBIndex> index = IDBIndex::create(info, objectStore);

    auto operation = createTransactionOperation(*this, &IDBTransaction::didCreateIndexOnServer, &IDBTransaction::createIndexOnServer, info);
    scheduleOperation(WTF::move(operation));

    return WTF::move(index);
}

void IDBTransaction::createIndexOnServer(TransactionOperation& operation, const IDBIndexInfo& info)
{
    LOG(IndexedDB, "IDBTransaction::createIndexOnServer");

    ASSERT(isVersionChange());

    m_database->serverConnection().createIndex(operation, info);
}

void IDBTransaction::didCreateIndexOnServer(const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didCreateIndexOnServer");

    ASSERT_UNUSED(resultData, resultData.type() == IDBResultType::CreateIndexSuccess);
}

Ref<IDBRequest> IDBTransaction::requestGetRecord(ScriptExecutionContext& context, IDBObjectStore& objectStore, const IDBKeyRangeData& keyRangeData)
{
    LOG(IndexedDB, "IDBTransaction::requestGetRecord");
    ASSERT(isActive());
    ASSERT(!keyRangeData.isNull);

    Ref<IDBRequest> request = IDBRequest::create(context, objectStore, *this);
    addRequest(request.get());

    auto operation = createTransactionOperation(*this, request.get(), &IDBTransaction::didGetRecordOnServer, &IDBTransaction::getRecordOnServer, keyRangeData);
    scheduleOperation(WTF::move(operation));

    return WTF::move(request);
}

Ref<IDBRequest> IDBTransaction::requestGetValue(ScriptExecutionContext& context, IDBIndex& index, const IDBKeyRangeData& range)
{
    LOG(IndexedDB, "IDBTransaction::requestGetValue");
    return requestIndexRecord(context, index, IndexedDB::IndexRecordType::Value, range);
}

Ref<IDBRequest> IDBTransaction::requestGetKey(ScriptExecutionContext& context, IDBIndex& index, const IDBKeyRangeData& range)
{
    LOG(IndexedDB, "IDBTransaction::requestGetValue");
    return requestIndexRecord(context, index, IndexedDB::IndexRecordType::Key, range);
}

Ref<IDBRequest> IDBTransaction::requestIndexRecord(ScriptExecutionContext& context, IDBIndex& index, IndexedDB::IndexRecordType type, const IDBKeyRangeData&range)
{
    LOG(IndexedDB, "IDBTransaction::requestGetValue");
    ASSERT(isActive());
    ASSERT(!range.isNull);

    Ref<IDBRequest> request = IDBRequest::createGet(context, index, type, *this);
    addRequest(request.get());

    auto operation = createTransactionOperation(*this, request.get(), &IDBTransaction::didGetRecordOnServer, &IDBTransaction::getRecordOnServer, range);
    scheduleOperation(WTF::move(operation));

    return WTF::move(request);
}

void IDBTransaction::getRecordOnServer(TransactionOperation& operation, const IDBKeyRangeData& keyRange)
{
    LOG(IndexedDB, "IDBTransaction::getRecordOnServer");

    serverConnection().getRecord(operation, keyRange);
}

void IDBTransaction::didGetRecordOnServer(IDBRequest& request, const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didGetRecordOnServer");

    const IDBGetResult& result = resultData.getResult();

    if (request.sourceIndexIdentifier() && request.requestedIndexRecordType() == IndexedDB::IndexRecordType::Key) {
        if (!result.keyData.isNull())
            request.setResult(&result.keyData);
        else
            request.setResultToUndefined();
    } else {
        if (resultData.getResult().valueBuffer.data())
            request.setResultToStructuredClone(resultData.getResult().valueBuffer);
        else
            request.setResultToUndefined();
    }

    request.requestCompleted(resultData);
}

Ref<IDBRequest> IDBTransaction::requestCount(ScriptExecutionContext& context, IDBObjectStore& objectStore, const IDBKeyRangeData& range)
{
    LOG(IndexedDB, "IDBTransaction::requestCount (IDBObjectStore)");
    ASSERT(isActive());
    ASSERT(!range.isNull);

    Ref<IDBRequest> request = IDBRequest::create(context, objectStore, *this);
    addRequest(request.get());

    scheduleOperation(createTransactionOperation(*this, request.get(), &IDBTransaction::didGetCountOnServer, &IDBTransaction::getCountOnServer, range));

    return request;
}

Ref<IDBRequest> IDBTransaction::requestCount(ScriptExecutionContext& context, IDBIndex& index, const IDBKeyRangeData& range)
{
    LOG(IndexedDB, "IDBTransaction::requestCount (IDBIndex)");
    ASSERT(isActive());
    ASSERT(!range.isNull);

    Ref<IDBRequest> request = IDBRequest::createCount(context, index, *this);
    addRequest(request.get());

    scheduleOperation(createTransactionOperation(*this, request.get(), &IDBTransaction::didGetCountOnServer, &IDBTransaction::getCountOnServer, range));

    return request;
}

void IDBTransaction::getCountOnServer(TransactionOperation& operation, const IDBKeyRangeData& keyRange)
{
    LOG(IndexedDB, "IDBTransaction::getCountOnServer");

    serverConnection().getCount(operation, keyRange);
}

void IDBTransaction::didGetCountOnServer(IDBRequest& request, const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didGetCountOnServer");

    request.setResult(resultData.resultInteger());
    request.requestCompleted(resultData);
}

Ref<IDBRequest> IDBTransaction::requestDeleteRecord(ScriptExecutionContext& context, IDBObjectStore& objectStore, const IDBKeyRangeData& range)
{
    LOG(IndexedDB, "IDBTransaction::requestDeleteRecord");
    ASSERT(isActive());
    ASSERT(!range.isNull);

    Ref<IDBRequest> request = IDBRequest::create(context, objectStore, *this);
    addRequest(request.get());

    scheduleOperation(createTransactionOperation(*this, request.get(), &IDBTransaction::didDeleteRecordOnServer, &IDBTransaction::deleteRecordOnServer, range));
    return request;
}

void IDBTransaction::deleteRecordOnServer(TransactionOperation& operation, const IDBKeyRangeData& keyRange)
{
    LOG(IndexedDB, "IDBTransaction::deleteRecordOnServer");

    serverConnection().deleteRecord(operation, keyRange);
}

void IDBTransaction::didDeleteRecordOnServer(IDBRequest& request, const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didDeleteRecordOnServer");

    request.setResultToUndefined();
    request.requestCompleted(resultData);
}

Ref<IDBRequest> IDBTransaction::requestClearObjectStore(ScriptExecutionContext& context, IDBObjectStore& objectStore)
{
    LOG(IndexedDB, "IDBTransaction::requestClearObjectStore");
    ASSERT(isActive());

    Ref<IDBRequest> request = IDBRequest::create(context, objectStore, *this);
    addRequest(request.get());

    uint64_t objectStoreIdentifier = objectStore.info().identifier();
    auto operation = createTransactionOperation(*this, request.get(), &IDBTransaction::didClearObjectStoreOnServer, &IDBTransaction::clearObjectStoreOnServer, objectStoreIdentifier);
    scheduleOperation(WTF::move(operation));

    return WTF::move(request);
}

void IDBTransaction::clearObjectStoreOnServer(TransactionOperation& operation, const uint64_t& objectStoreIdentifier)
{
    LOG(IndexedDB, "IDBTransaction::clearObjectStoreOnServer");

    serverConnection().clearObjectStore(operation, objectStoreIdentifier);
}

void IDBTransaction::didClearObjectStoreOnServer(IDBRequest& request, const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didClearObjectStoreOnServer");

    request.setResultToUndefined();
    request.requestCompleted(resultData);
}

Ref<IDBRequest> IDBTransaction::requestPutOrAdd(ScriptExecutionContext& context, IDBObjectStore& objectStore, IDBKey* key, SerializedScriptValue& value, IndexedDB::ObjectStoreOverwriteMode overwriteMode)
{
    LOG(IndexedDB, "IDBTransaction::requestPutOrAdd");
    ASSERT(isActive());
    ASSERT(!isReadOnly());
    ASSERT(objectStore.info().autoIncrement() || key);

    Ref<IDBRequest> request = IDBRequest::create(context, objectStore, *this);
    addRequest(request.get());

    auto operation = createTransactionOperation(*this, request.get(), &IDBTransaction::didPutOrAddOnServer, &IDBTransaction::putOrAddOnServer, key, &value, overwriteMode);
    scheduleOperation(WTF::move(operation));

    return WTF::move(request);
}

void IDBTransaction::putOrAddOnServer(TransactionOperation& operation, RefPtr<IDBKey> key, RefPtr<SerializedScriptValue> value, const IndexedDB::ObjectStoreOverwriteMode& overwriteMode)
{
    LOG(IndexedDB, "IDBTransaction::putOrAddOnServer");

    ASSERT(!isReadOnly());

    serverConnection().putOrAdd(operation, key, value, overwriteMode);
}

void IDBTransaction::didPutOrAddOnServer(IDBRequest& request, const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didPutOrAddOnServer");

    request.setResult(resultData.resultKey());
    request.requestCompleted(resultData);
}

void IDBTransaction::deleteObjectStore(const String& objectStoreName)
{
    LOG(IndexedDB, "IDBTransaction::deleteObjectStore");

    ASSERT(isVersionChange());

    if (auto objectStore = m_referencedObjectStores.take(objectStoreName))
        objectStore->markAsDeleted();

    auto operation = createTransactionOperation(*this, &IDBTransaction::didDeleteObjectStoreOnServer, &IDBTransaction::deleteObjectStoreOnServer, objectStoreName);
    scheduleOperation(WTF::move(operation));
}

void IDBTransaction::deleteObjectStoreOnServer(TransactionOperation& operation, const String& objectStoreName)
{
    LOG(IndexedDB, "IDBTransaction::deleteObjectStoreOnServer");
    ASSERT(isVersionChange());

    serverConnection().deleteObjectStore(operation, objectStoreName);
}

void IDBTransaction::didDeleteObjectStoreOnServer(const IDBResultData& resultData)
{
    LOG(IndexedDB, "IDBTransaction::didDeleteObjectStoreOnServer");
    ASSERT_UNUSED(resultData, resultData.type() == IDBResultType::DeleteObjectStoreSuccess);
}

void IDBTransaction::operationDidComplete(TransactionOperation& operation)
{
    ASSERT(m_transactionOperationMap.get(operation.identifier()) == &operation);
    m_transactionOperationMap.remove(operation.identifier());

    scheduleOperationTimer();
}

void IDBTransaction::establishOnServer()
{
    LOG(IndexedDB, "IDBTransaction::establishOnServer");

    serverConnection().establishTransaction(*this);
}

void IDBTransaction::activate()
{
    if (isFinishedOrFinishing())
        return;

    m_state = IndexedDB::TransactionState::Active;
}

void IDBTransaction::deactivate()
{
    if (m_state == IndexedDB::TransactionState::Active)
        m_state = IndexedDB::TransactionState::Inactive;

    scheduleOperationTimer();
}

} // namespace IDBClient
} // namespace WebCore

#endif // ENABLE(INDEXED_DATABASE)
