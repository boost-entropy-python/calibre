/*
 * winspeech.cpp
 * Copyright (C) 2023 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */
#include "common.h"

#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Media::SpeechSynthesis;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Storage::Streams;
typedef unsigned long long id_type;

static PyObject*
runtime_error_as_python_error(PyObject *exc_type, winrt::hresult_error const &ex, const char *file, const int line, const char *prefix="", PyObject *name=NULL) {
    pyobject_raii msg(PyUnicode_FromWideChar(ex.message().c_str(), -1));
    const HRESULT hr = ex.to_abi();
    if (name) PyErr_Format(exc_type, "%s:%d:%s:[hr=0x%x] %V: %S", file, line, prefix, hr, msg.ptr(), "Out of memory", name);
    else PyErr_Format(exc_type, "%s:%d:%s:[hr=0x%x] %V", file, line, prefix, hr, msg.ptr(), "Out of memory");
    return NULL;
}

#define CATCH_ALL_EXCEPTIONS(msg) catch(winrt::hresult_error const& ex) { \
    runtime_error_as_python_error(PyExc_OSError, ex, __FILE__, __LINE__, msg); \
} catch (std::exception const &ex) { \
    PyErr_Format(PyExc_OSError, "%s:%d:%s: %s", __FILE__, __LINE__, msg, ex.what()); \
} catch (...) { \
    PyErr_Format(PyExc_OSError, "%s:%d:%s: Unknown exception type was raised", __FILE__, __LINE__, msg); \
}


template<typename T>
class WeakRefs {
    private:
    std::mutex weak_ref_lock;
    std::unordered_map<id_type, T*> refs;
    id_type counter;
    public:
    id_type register_ref(T *self) {
        std::scoped_lock lock(weak_ref_lock);
        auto id = ++counter;
        refs[id] = self;
        return id;
    }
    void unregister_ref(T* self) {
        std::scoped_lock lock(weak_ref_lock);
        auto id = self->clear_id();
        refs.erase(id);
        self->~T();
    }
    void use_ref(id_type id, std::function<void(T*)> callback) {
        std::scoped_lock lock(weak_ref_lock);
        try {
            callback(refs.at(id));
        } catch (std::out_of_range) {
            callback(NULL);
        }
    }
};

enum class EventType {
    playback_state_changed = 1, media_opened, media_failed, media_ended, source_changed, cue_entered, cue_exited, track_failed
};

class Event {
    private:
        EventType type;
    public:
        Event(EventType type) : type(type) {}
        Event(const Event &source) : type(source.type) {}
};

class SynthesizerImplementation {
    private:
    id_type id;
    DWORD creation_thread_id;
    SpeechSynthesizer synth{nullptr};
    MediaPlayer player{nullptr};
    MediaSource current_source{nullptr};
    SpeechSynthesisStream current_stream{nullptr};
    MediaPlaybackItem currently_playing{nullptr};

    struct {
        MediaPlaybackSession::PlaybackStateChanged_revoker playback_state_changed;
        MediaPlayer::MediaEnded_revoker media_ended; MediaPlayer::MediaOpened_revoker media_opened;
        MediaPlayer::MediaFailed_revoker media_failed; MediaPlayer::SourceChanged_revoker source_changed;

        MediaPlaybackItem::TimedMetadataTracksChanged_revoker timed_metadata_tracks_changed;
        std::vector<TimedMetadataTrack::CueEntered_revoker> cue_entered;
        std::vector<TimedMetadataTrack::CueExited_revoker> cue_exited;
        std::vector<TimedMetadataTrack::TrackFailed_revoker> track_failed;
    } revoker;

    std::vector<Event> events;
    std::mutex events_lock;

    public:
    SynthesizerImplementation();

    void add_simple_event(EventType type) {
        try {
            std::scoped_lock lock(events_lock);
            events.emplace_back(type);
        } catch(...) {}
    }

    SpeechSynthesisStream synthesize(const std::wstring_view &text, bool is_ssml = false) {
        if (is_ssml) return synth.SynthesizeSsmlToStreamAsync(text).get();
        return synth.SynthesizeTextToStreamAsync(text).get();
    }

    void speak(const std::wstring_view &text, bool is_ssml = false) {
        revoker.cue_entered.clear();
        revoker.cue_exited.clear();
        revoker.track_failed.clear();
        current_stream = synthesize(text, is_ssml);
        current_source = MediaSource::CreateFromStream(current_stream, current_stream.ContentType());
        currently_playing = MediaPlaybackItem(current_source);
        auto self_id = id;
        revoker.timed_metadata_tracks_changed = currently_playing.TimedMetadataTracksChanged(winrt::auto_revoke, [self_id](auto, auto const &args) {
            auto change_type = args.CollectionChange();
            auto index = args.Index();
            synthesizer_weakrefs.use_ref(self_id, [change_type, index](auto s) {
            if (!s) return;
            switch (change_type) {
            case CollectionChange::ItemInserted: {
                s->register_metadata_handler_for_speech(s->currently_playing.TimedMetadataTracks().GetAt(index));
                } break;
            case CollectionChange::Reset:
                for (auto const& track : s->currently_playing.TimedMetadataTracks()) {
                    s->register_metadata_handler_for_speech(track);
                }
                break;
            }});
        });
        player.Source(currently_playing);
        for (auto const &track : currently_playing.TimedMetadataTracks()) {
            register_metadata_handler_for_speech(track);
        }
    }

    bool is_creation_thread() const noexcept {
        return creation_thread_id == GetCurrentThreadId();
    }

    id_type clear_id() noexcept {
        auto ans = id;
        id = 0;
        return ans;
    }

    void register_metadata_handler_for_speech(TimedMetadataTrack const& track) {
        fprintf(stderr, "99999999999 registering metadata handler\n");
        auto self_id = id;
#define simple_event_listener(method, event_type) \
        revoker.event_type.push_back(method(winrt::auto_revoke, [self_id](auto, const auto&) { \
        fprintf(stderr, "111111111 %s %u\n", #event_type, GetCurrentThreadId()); fflush(stderr); \
        synthesizer_weakrefs.use_ref(self_id, [](auto s) { \
            if (!s) return; \
            s->add_simple_event(EventType::event_type); \
            fprintf(stderr, "2222222222 %d\n", s->player.PlaybackSession().PlaybackState()); \
        }); \
    }));
        simple_event_listener(track.CueEntered, cue_entered);
        simple_event_listener(track.CueExited, cue_exited);
        simple_event_listener(track.TrackFailed, track_failed);
#undef simple_event_listener
        track.CueEntered([](auto, const auto&) {
            fprintf(stderr, "cue entered\n"); fflush(stderr);
        });
}


};

struct Synthesizer {
    PyObject_HEAD
    SynthesizerImplementation impl;
};

static PyTypeObject SynthesizerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
};

static WeakRefs<SynthesizerImplementation> synthesizer_weakrefs;

SynthesizerImplementation::SynthesizerImplementation() {
    events.reserve(128);
    synth = SpeechSynthesizer();
    synth.Options().IncludeSentenceBoundaryMetadata(true);
    synth.Options().IncludeWordBoundaryMetadata(true);
    player = MediaPlayer();
    player.AudioCategory(MediaPlayerAudioCategory::Speech);
    player.AutoPlay(true);
    creation_thread_id = GetCurrentThreadId();
    id = synthesizer_weakrefs.register_ref(this);
    auto self_id = id;
#define simple_event_listener(method, event_type) \
        revoker.event_type = method(winrt::auto_revoke, [self_id](auto, const auto&) { \
        fprintf(stderr, "111111111 %s %u\n", #event_type, GetCurrentThreadId()); fflush(stderr); \
        synthesizer_weakrefs.use_ref(self_id, [](auto s) { \
            if (!s) return; \
            s->add_simple_event(EventType::event_type); \
            fprintf(stderr, "2222222222 %d\n", s->player.PlaybackSession().PlaybackState()); \
        }); \
    });
    simple_event_listener(player.PlaybackSession().PlaybackStateChanged, playback_state_changed);
    simple_event_listener(player.MediaOpened, media_opened);
    simple_event_listener(player.MediaFailed, media_failed);
    simple_event_listener(player.MediaEnded, media_ended);
    simple_event_listener(player.SourceChanged, source_changed);
#undef simple_event_listener
    player.PlaybackSession().PlaybackStateChanged([](auto, const auto&) {
        fprintf(stderr, "111111111 %s %u\n", "playback state changed", GetCurrentThreadId()); fflush(stderr); \
    });
    player.MediaOpened([](auto, const auto&) {
        fprintf(stderr, "111111111 %s %u\n", "media opened", GetCurrentThreadId()); fflush(stderr); \
    });
    player.MediaFailed([](auto, const auto&) {
        fprintf(stderr, "111111111 %s %u\n", "media failed", GetCurrentThreadId()); fflush(stderr); \
    });
    player.MediaEnded([](auto, const auto&) {
        fprintf(stderr, "111111111 %s %u\n", "media ended", GetCurrentThreadId()); fflush(stderr); \
    });
}

static PyObject*
Synthesizer_new(PyTypeObject *type, PyObject *args, PyObject *kwds) { INITIALIZE_COM_IN_FUNCTION
	Synthesizer *self = (Synthesizer *) type->tp_alloc(type, 0);
    if (self) {
        auto i = &self->impl;
        try {
            new (i) SynthesizerImplementation();
        } CATCH_ALL_EXCEPTIONS("Failed to create SynthesizerImplementation object");
        if (PyErr_Occurred()) { Py_CLEAR(self); }
    }
    if (self) com.detach();
    return (PyObject*)self;
}

static void
Synthesizer_dealloc(Synthesizer *self) {
    auto *i = &self->impl;
    try {
        synthesizer_weakrefs.unregister_ref(i);
    } CATCH_ALL_EXCEPTIONS("Failed to destruct SynthesizerImplementation");
    if (PyErr_Occurred()) { PyErr_Print(); }
    Py_TYPE(self)->tp_free((PyObject*)self);
    CoUninitialize();
}

static void
ensure_current_thread_has_message_queue(void) {
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
}

#define PREPARE_METHOD_CALL ensure_current_thread_has_message_queue(); if (!self->impl.is_creation_thread()) { PyErr_SetString(PyExc_RuntimeError, "Cannot use a Synthesizer object from a thread other than the thread it was created in"); return NULL; }

static PyObject*
Synthesizer_speak(Synthesizer *self, PyObject *args) {
    PREPARE_METHOD_CALL;
    wchar_raii pytext;
    int is_ssml = 0;
	if (!PyArg_ParseTuple(args, "O&|p", py_to_wchar_no_none, &pytext, &is_ssml)) return NULL;
    try {
        self->impl.speak(pytext.as_view(), (bool)is_ssml);
    } CATCH_ALL_EXCEPTIONS("Failed to start speaking text");
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}


static PyObject*
Synthesizer_create_recording(Synthesizer *self, PyObject *args) {
    PREPARE_METHOD_CALL;
    wchar_raii pytext;
    PyObject *callback;
    int is_ssml = 0;
	if (!PyArg_ParseTuple(args, "O&O|p", py_to_wchar_no_none, &pytext, &callback, &is_ssml)) return NULL;
    if (!PyCallable_Check(callback)) { PyErr_SetString(PyExc_TypeError, "callback must be callable"); return NULL; }

    SpeechSynthesisStream stream{nullptr};
    try {
        stream = self->impl.synthesize(pytext.as_view(), (bool)is_ssml);
    } CATCH_ALL_EXCEPTIONS( "Failed to get SpeechSynthesisStream from text");
    if (PyErr_Occurred()) return NULL;
    unsigned long long stream_size = stream.Size(), bytes_read = 0;
    DataReader reader(stream);
    unsigned int n;
    const static unsigned int chunk_size = 16 * 1024;
    while (bytes_read < stream_size) {
        try {
            n = reader.LoadAsync(chunk_size).get();
        } CATCH_ALL_EXCEPTIONS("Failed to load data from DataReader");
        if (PyErr_Occurred()) return NULL;
        if (n > 0) {
            bytes_read += n;
            pyobject_raii b(PyBytes_FromStringAndSize(NULL, n));
            if (!b) return NULL;
            unsigned char *p = reinterpret_cast<unsigned char*>(PyBytes_AS_STRING(b.ptr()));
            reader.ReadBytes(winrt::array_view(p, p + n));
            pyobject_raii ret(PyObject_CallFunctionObjArgs(callback, b.ptr(), NULL));
        }
    }

    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}


static PyObject*
voice_as_dict(VoiceInformation const& voice) {
    try {
        const char *gender = "";
        switch (voice.Gender()) {
            case VoiceGender::Male: gender = "male"; break;
            case VoiceGender::Female: gender = "female"; break;
        }
        return Py_BuildValue("{su su su su ss}",
            "display_name", voice.DisplayName().c_str(),
            "description", voice.Description().c_str(),
            "id", voice.Id().c_str(),
            "language", voice.Language().c_str(),
            "gender", gender
        );
    } CATCH_ALL_EXCEPTIONS("Could not convert Voice to dict");
    return NULL;
}


static PyObject*
all_voices(PyObject* /*self*/, PyObject* /*args*/) { INITIALIZE_COM_IN_FUNCTION
    try {
        auto voices = SpeechSynthesizer::AllVoices();
        pyobject_raii ans(PyTuple_New(voices.Size()));
        if (!ans) return NULL;
        Py_ssize_t i = 0;
        for(auto const& voice : voices) {
            PyObject *v = voice_as_dict(voice);
            if (v) {
                PyTuple_SET_ITEM(ans.ptr(), i++, v);
            } else {
                return NULL;
            }
        }
        return ans.detach();
    } CATCH_ALL_EXCEPTIONS("Could not get all voices");
    return NULL;
}

static PyObject*
default_voice(PyObject* /*self*/, PyObject* /*args*/) { INITIALIZE_COM_IN_FUNCTION
    try {
        return voice_as_dict(SpeechSynthesizer::DefaultVoice());
    } CATCH_ALL_EXCEPTIONS("Could not get default voice");
    return NULL;
}

#define M(name, args) { #name, (PyCFunction)Synthesizer_##name, args, ""}
static PyMethodDef Synthesizer_methods[] = {
    M(create_recording, METH_VARARGS),
    M(speak, METH_VARARGS),
    {NULL, NULL, 0, NULL}
};
#undef M

static PyObject*
pump_waiting_messages(PyObject*, PyObject*) {
	UINT firstMsg = 0, lastMsg = 0;
    MSG msg;
    bool found = false;
	// Read all of the messages in this next loop,
	// removing each message as we read it.
	while (PeekMessage(&msg, NULL, firstMsg, lastMsg, PM_REMOVE)) {
		// If it's a quit message, we're out of here.
		if (msg.message == WM_QUIT) {
            Py_RETURN_NONE;
		}
        found = true;
		// Otherwise, dispatch the message.
		DispatchMessage(&msg);
	} // End of PeekMessage while loop

    if (found) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}


#define M(name, args) { #name, name, args, ""}
static PyMethodDef methods[] = {
    M(all_voices, METH_NOARGS),
    M(default_voice, METH_NOARGS),
    M(pump_waiting_messages, METH_NOARGS),
    {NULL, NULL, 0, NULL}
};
#undef M


static int
exec_module(PyObject *m) {
    SynthesizerType.tp_name = "winspeech.Synthesizer";
    SynthesizerType.tp_doc = "Wrapper for SpeechSynthesizer";
    SynthesizerType.tp_basicsize = sizeof(Synthesizer);
    SynthesizerType.tp_itemsize = 0;
    SynthesizerType.tp_flags = Py_TPFLAGS_DEFAULT;
    SynthesizerType.tp_new = Synthesizer_new;
    SynthesizerType.tp_methods = Synthesizer_methods;
	SynthesizerType.tp_dealloc = (destructor)Synthesizer_dealloc;
	if (PyType_Ready(&SynthesizerType) < 0) return -1;

	Py_INCREF(&SynthesizerType);
    if (PyModule_AddObject(m, "Synthesizer", (PyObject *) &SynthesizerType) < 0) {
        Py_DECREF(&SynthesizerType);
        return -1;
    }

    return 0;
}

static PyModuleDef_Slot slots[] = { {Py_mod_exec, (void*)exec_module}, {0, NULL} };

static struct PyModuleDef module_def = {PyModuleDef_HEAD_INIT};

CALIBRE_MODINIT_FUNC PyInit_winspeech(void) {
    module_def.m_name     = "winspeech";
    module_def.m_doc      = "Windows Speech API wrapper";
    module_def.m_methods  = methods;
    module_def.m_slots    = slots;
	return PyModuleDef_Init(&module_def);
}
