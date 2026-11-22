#pragma once

void ResponsesMessagePhases(const std::string& delivery, int end_turn) {
    const bool streaming = delivery != "json";
    const bool item_only = delivery == "item" || delivery == "item_only";
    const bool anonymous = delivery == "anonymous" || delivery == "anonymous_item_only";
    const auto message = [](const char* id, const char* phase, const char* first, const char* second) {
        return Json{{"id", id}, {"type", "message"}, {"role", "assistant"}, {"phase", phase},
            {"content", Json::array({{{"type", "output_text"}, {"text", first}},
                                    {{"type", "output_text"}, {"text", second}}})}};
    };
    const Json reasoning = {{"id", "r-1"}, {"type", "reasoning"},
        {"summary", Json::array({{{"type", "summary_text"}, {"text", "Thinking"}}})},
        {"encrypted_content", "opaque"}};
    const Json call = {{"id", "fc-1"}, {"type", "function_call"}, {"call_id", "call-1"},
        {"name", "read_file"}, {"arguments", "{\"path\":\"README.md\"}"}};
    const Json output = Json::array({reasoning, message("m-1", "commentary", "Working", " now"),
        call, message("m-2", "final_answer", "Done", "!")});
    Json response = {{"status", "completed"}, {"output", output}};
    if (end_turn >= 0) response["end_turn"] = end_turn != 0;
    std::string body;
    if (!streaming) {
        body = response.dump();
    } else {
        if (anonymous) {
            body += Event("response.output_text.delta", {{"delta", "Work"}});
            body += Event("response.output_item.added",
                {{"item", {{"type", "web_search_call"}, {"action", nullptr}}}});
            for (auto item : output) {
                if (item["type"] == "message") item.erase("id");
                body += Event("response.output_item.added", {{"item", item}});
                body += Event("response.output_item.done", {{"item", item}});
            }
        } else if (delivery == "id_only") {
            for (const auto& item : output) {
                if (item["type"] != "message") continue;
                for (size_t part = 0; part < item["content"].size(); ++part) {
                    body += Event("response.output_text.delta",
                        {{"item_id", item["id"]}, {"content_index", part},
                         {"delta", item["content"][part]["text"]}});
                }
            }
        } else if (delivery == "legacy" || delivery == "legacy_full" || delivery == "legacy_mixed") {
            body += Event("response.output_text.delta", {{"delta", delivery == "legacy_full" ? "Working now" : "Work"}});
            if (delivery == "legacy_mixed") {
                body += Event("response.output_text.done", {{"output_index", 1}, {"content_index", 0}, {"text", "Working"}});
            }
        } else if (delivery != "completed") {
            for (size_t index = 0; index < output.size(); ++index) {
                auto item = output[index];
                if (!item_only && delivery != "done") {
                    if (item["type"] == "message") item["content"] = Json::array();
                    Json added = {{"output_index", index}, {"item", item}};
                    if (delivery == "unindexed") added.erase("output_index");
                    body += Event("response.output_item.added", added);
                }
                if (output[index]["type"] == "message" && !item_only) {
                    for (size_t step = 0; step < 2; ++step) {
                        const size_t part = delivery == "reverse" ? 1 - step : step;
                        const std::string text = output[index]["content"][part]["text"];
                        Json event = {{"output_index", index}, {"content_index", part},
                            {"item_id", output[index]["id"]}};
                        if (delivery == "unindexed") {
                            event.erase("output_index");
                            event.erase("item_id");
                        }
                        if (delivery == "flat_indexed") event.erase("content_index");
                        if (delivery != "done" && delivery != "done_only") {
                            event["delta"] = delivery.rfind("partial", 0) == 0 ? text.substr(0, 2) : text;
                            event["sequence_number"] = index * 10 + part;
                            body += Event("response.output_text.delta", event);
                            body += Event("response.output_text.delta", event);
                        }
                        event.erase("delta");
                        event["text"] = text;
                        if (delivery != "partial_item" && delivery != "partial_completed" &&
                            delivery != "flat_indexed") {
                            body += Event("response.output_text.done", event);
                        }
                    }
                    if (delivery == "flat_indexed") {
                        const auto& content = output[index]["content"];
                        body += Event("response.output_text.done",
                            {{"output_index", index}, {"item_id", output[index]["id"]},
                             {"text", content[0]["text"].get<std::string>() +
                                      content[1]["text"].get<std::string>()}});
                    }
                }
                Json done = {{"output_index", index}, {"item", output[index]}};
                if (delivery == "unindexed") done.erase("output_index");
                if (delivery != "partial_completed" &&
                    (delivery != "done_only" || output[index]["type"] != "message")) {
                    body += Event("response.output_item.done", done);
                }
            }
        }
        auto completed = response;
        if (delivery == "item_only" || delivery == "done_only" ||
            delivery == "anonymous_item_only") completed.erase("output");
        body += Event("response.completed", {{"response", completed}});
    }
    Server server([&](Server::Connection& connection, size_t attempt) {
        const auto reply = attempt == 0 ? body : Reply("responses", streaming);
        connection.Send(Server::Headers(200, reply.size(),
            streaming ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n"));
        for (size_t offset = 0; offset < reply.size(); offset += 11) {
            if (!connection.Send(std::string_view(reply).substr(offset, 11))) return;
        }
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = streaming;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.messages.push_back({MessageRole::User, "Review"});
    std::string displayed, thinking;
    ProviderCallbacks callbacks;
    callbacks.on_text_delta = [&](const auto& text) { displayed += text; };
    callbacks.on_reasoning_delta = [&](const auto& text) { thinking += text; };
    auto adapter = CreateProviderAdapter(provider.protocol);
    const auto first = adapter->Complete(request, callbacks, nullptr);
    Check(first.ok && first.value.text == "Working now\n\nDone!" && displayed == first.value.text,
          "Responses indexed delta/done/item/completed merge messages and missing suffixes exactly once");
    Check(first.ok && first.value.needs_follow_up == (end_turn == 0),
          "Responses follows up only on explicit end_turn=false");
    if (!first.ok) { std::cerr << delivery << ": " << first.error << '\n'; return; }
    const Json expected = Json::array({
        {{"role", "assistant"}, {"content", "Working now"}, {"phase", "commentary"}},
        {{"role", "assistant"}, {"content", "Done!"}, {"phase", "final_answer"}}});
    Check(Json(first.value.assistant_messages) == expected,
          "Responses preserves each message content and phase independently");
    Check(first.value.diagnostics.find(std::string("end_turn=") + (end_turn < 0 ? "absent" : end_turn ? "true" : "false")) != std::string::npos &&
          first.value.diagnostics.find("phases=commentary,final_answer") != std::string::npos,
          "Responses diagnostics distinguish end_turn and output message phases");
    Check(thinking == "Thinking" && first.value.reasoning_items == std::vector<std::string>{reasoning.dump()} &&
          first.value.tool_calls.size() == 1 && first.value.tool_calls[0].id == "call-1" &&
          first.value.tool_calls[0].arguments_json == "{\"path\":\"README.md\"}",
          "Responses message assembly preserves reasoning and tool results");
    const auto restored = Json(first.value.assistant_messages).get<std::vector<ChatMessage>>();
    request.messages.insert(request.messages.end(), restored.begin(), restored.end());
    ChatMessage unknown;
    unknown.role = MessageRole::Assistant;
    unknown.content = "Unknown phase";
    unknown.phase = "other";
    request.messages.push_back(unknown);
    Check(adapter->Complete(request, {}, nullptr).ok, "Responses accepts phase-preserving replay");
    const auto requests = server.Requests();
    if (requests.size() != 2) { Check(false, "phase round trip makes two requests"); return; }
    const auto input = Json::parse(requests.back().body)["input"];
    Check(input.size() == 4 && input[1].value("phase", "") == "commentary" &&
          input[2].value("phase", "") == "final_answer" && !input[3].contains("phase"),
          "Responses replays each recognized phase and omits unknown phases");
}

void ResponsesEmptyFollowUp(bool streaming, const std::string& kind, int end_turn) {
    const Json reasoning = {{"id", "r-empty"}, {"type", "reasoning"},
        {"summary", Json::array({{{"type", "summary_text"}, {"text", "Checking"}}})}};
    Json output = kind == "reasoning" ? Json::array({reasoning}) : Json::array();
    if (kind == "commentary") {
        output.push_back({{"type", "message"}, {"role", "assistant"}, {"phase", "commentary"},
            {"content", Json::array({{{"type", "output_text"}, {"text", "Working"}}})}});
    }
    Json response = {{"status", "completed"}, {"output", output}};
    if (end_turn >= 0) response["end_turn"] = end_turn != 0;
    const auto body = streaming ? Event("response.completed", {{"response", response}}) : response.dump();
    Server server([&](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, body.size(),
            streaming ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n") + body);
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = streaming;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    const auto reply = CreateProviderAdapter(provider.protocol)->Complete(request, {}, nullptr);
    Check(reply.ok == (end_turn == 0 || kind == "commentary"),
          "Responses accepts non-empty commentary and explicit continuation");
    if (reply.ok) {
        Check(reply.value.needs_follow_up == (end_turn == 0) &&
              reply.value.text == (kind == "commentary" ? "Working" : "") && reply.value.tool_calls.empty() &&
              reply.value.reasoning_items == (kind == "reasoning" ? std::vector<std::string>{reasoning.dump()} : std::vector<std::string>{}),
              "continuation preserves explicit end_turn semantics and opaque reasoning");
    }
}

void MessagePhaseCompatibility() {
    for (const std::string delivery : {"anonymous", "anonymous_item_only", "id_only", "flat_indexed"}) {
        for (int end_turn : {-1, 0, 1}) ResponsesMessagePhases(delivery, end_turn);
    }
    for (const Json phase : {Json("commentary"), Json("final_answer"), Json("other"), Json(nullptr), Json(1)}) {
        ChatMessage message;
        message.phase = "commentary";
        Json source = {{"role", "assistant"}, {"content", "Text"}, {"phase", phase}};
        from_json(source, message);
        const bool known = phase == "commentary" || phase == "final_answer";
        if (!known) source.erase("phase");
        Check(Json(message) == source && (known || message.phase.empty()),
              "message serialization retains only recognized phases and clears invalid old values");
    }
    ChatMessage old;
    old.phase = "final_answer";
    const Json legacy = {{"role", "assistant"}, {"content", "Legacy"}};
    from_json(legacy, old);
    Check(old.phase.empty() && Json(old) == legacy, "legacy messages retain unknown-phase compatibility");
    for (const std::string protocol : {"chat_completions", "anthropic_messages"}) {
        const auto body = Reply(protocol, false);
        Server server([&](Server::Connection& connection, size_t) {
            connection.Send(Server::Headers(200, body.size()) + body);
        });
        ProviderConfig provider;
        provider.protocol = protocol;
        provider.base_url = server.Url("/v1");
        ModelConfig model;
        model.id = "fixture-model";
        model.capabilities.streaming = false;
        ProviderRequest request;
        request.provider = &provider;
        request.model = &model;
        old.phase = "commentary";
        request.messages.push_back(old);
        Check(CreateProviderAdapter(protocol)->Complete(request, {}, nullptr).ok,
              "other provider protocols still accept assistant history");
        const auto requests = server.Requests();
        Check(requests.size() == 1 && requests.front().body.find("\"phase\"") == std::string::npos,
              "phase metadata is not injected into Chat Completions or Anthropic requests");
    }
}
