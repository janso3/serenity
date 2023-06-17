/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/SinglyLinkedList.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableStreamGenericReader.h>

namespace Web::Streams {

struct ReadableStreamReadResult {
    JS::Value value;
    bool done;
};

class ReadRequest : public RefCounted<ReadRequest> {
public:
    virtual ~ReadRequest() = default;

    virtual void on_chunk(JS::Value chunk) = 0;
    virtual void on_close() = 0;
    virtual void on_error(JS::Value error) = 0;
};

class ReadLoopReadRequest : public ReadRequest {
public:
    // successSteps, which is an algorithm accepting a byte sequence
    using SuccessSteps = JS::SafeFunction<void(ByteBuffer)>;

    // failureSteps, which is an algorithm accepting a JavaScript value
    using FailureSteps = JS::SafeFunction<void(JS::Value error)>;

    ReadLoopReadRequest(JS::VM& vm, JS::Realm& realm, ReadableStreamDefaultReader& reader, SuccessSteps success_steps, FailureSteps failure_steps);

    virtual void on_chunk(JS::Value chunk) override;

    virtual void on_close() override;

    virtual void on_error(JS::Value error) override;

private:
    JS::VM& m_vm;
    JS::Realm& m_realm;
    ReadableStreamDefaultReader& m_reader;
    ByteBuffer m_bytes;
    SuccessSteps m_success_steps;
    FailureSteps m_failure_steps;
};

// https://streams.spec.whatwg.org/#readablestreamdefaultreader
class ReadableStreamDefaultReader final
    : public Bindings::PlatformObject
    , public ReadableStreamGenericReaderMixin {
    WEB_PLATFORM_OBJECT(ReadableStreamDefaultReader, Bindings::PlatformObject);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStreamDefaultReader>> construct_impl(JS::Realm&, JS::NonnullGCPtr<ReadableStream>);

    virtual ~ReadableStreamDefaultReader() override = default;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> read();
    WebIDL::ExceptionOr<void> release_lock();

    SinglyLinkedList<NonnullRefPtr<ReadRequest>>& read_requests() { return m_read_requests; }

private:
    explicit ReadableStreamDefaultReader(JS::Realm&);

    virtual JS::ThrowCompletionOr<void> initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    SinglyLinkedList<NonnullRefPtr<ReadRequest>> m_read_requests;
};

}
