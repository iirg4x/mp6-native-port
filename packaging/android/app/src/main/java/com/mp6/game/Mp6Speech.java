/* MP6 native port -- Android half of the GameCube Microphone's speech
 * backend (include/mp6_mic.h, src/mic/speech_android.c).
 *
 * Wraps android.speech.SpeechRecognizer, the device's own recognizer
 * service (Google's on most phones, Samsung's on some), which captures the
 * microphone itself. The game's word list is passed as biasing strings on
 * Android 13+, and every hypothesis goes back to native code as text, where
 * the shared matcher maps it onto the minigame's words.
 *
 * SpeechRecognizer must be created and driven on the main thread; native
 * code calls these static methods from the game thread, so every call posts
 * to the main looper. The record permission is requested natively through
 * SDL before start() is ever called. Natives are registered by
 * speech_android.c (the game library is not loaded through
 * System.loadLibrary, so JNI's name lookup cannot find them on its own). */
package com.mp6.game;

import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;

import java.util.ArrayList;
import java.util.Arrays;

import org.libsdl.app.SDL;

public final class Mp6Speech {
    static native void nativeResult(String[] alternatives, float[] confidences, boolean isFinal);
    static native void nativeStopped(String reason);
    static native void nativeError(String message);

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static SpeechRecognizer sRecognizer;
    private static String[] sPhrases = new String[0];
    private static boolean sContinuous;
    private static boolean sWanted;   // the game is listening
    private static boolean sRunning;  // a startListening() is in flight
    private static int sSession;

    private Mp6Speech() {}

    private static Context context() {
        return SDL.getContext();
    }

    /** Cheap, prompt-free: is there a recognition service at all? */
    public static boolean available() {
        Context ctx = context();
        return ctx != null && SpeechRecognizer.isRecognitionAvailable(ctx);
    }

    public static void start(final String[] phrases, final boolean continuous) {
        sMain.post(() -> {
            sPhrases = phrases != null ? phrases : new String[0];
            sContinuous = continuous;
            sWanted = true;
            sSession++;
            listen();
        });
    }

    /** Talk button released: end the utterance and deliver its result. */
    public static void finish() {
        sMain.post(() -> {
            if (sRecognizer != null && sRunning) sRecognizer.stopListening();
            sContinuous = false;
        });
    }

    public static void stop() {
        sMain.post(() -> {
            sWanted = false;
            sSession++;
            if (sRecognizer != null) sRecognizer.cancel();
            sRunning = false;
        });
    }

    private static void listen() {
        Context ctx = context();
        if (ctx == null || !sWanted) return;
        if (sRecognizer == null) {
            try {
                sRecognizer = SpeechRecognizer.createSpeechRecognizer(ctx);
            } catch (RuntimeException e) {
                nativeError("Speech recognition is not available on this device");
                return;
            }
            sRecognizer.setRecognitionListener(new Listener());
        }
        Intent intent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        intent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        intent.putExtra(RecognizerIntent.EXTRA_LANGUAGE, "en-US");
        intent.putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 5);
        intent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
        intent.putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true);
        intent.putExtra(RecognizerIntent.EXTRA_CALLING_PACKAGE, ctx.getPackageName());
        if (Build.VERSION.SDK_INT >= 33 && sPhrases.length > 0) {
            intent.putStringArrayListExtra(RecognizerIntent.EXTRA_BIASING_STRINGS,
                    new ArrayList<>(Arrays.asList(sPhrases)));
        }
        try {
            sRecognizer.startListening(intent);
            sRunning = true;
        } catch (RuntimeException e) {
            sRunning = false;
            nativeStopped("startListening failed: " + e.getMessage());
        }
    }

    private static void deliver(Bundle bundle, boolean isFinal) {
        ArrayList<String> texts = bundle != null
                ? bundle.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION) : null;
        float[] scores = bundle != null ? bundle.getFloatArray(SpeechRecognizer.CONFIDENCE_SCORES) : null;
        String[] alternatives = texts != null ? texts.toArray(new String[0]) : new String[0];
        if (!isFinal && alternatives.length == 0) return;
        nativeResult(alternatives, scores != null && scores.length == alternatives.length ? scores : null, isFinal);
    }

    /** After an utterance: keep going in continuous mode. */
    private static void utteranceDone(final int session) {
        sRunning = false;
        if (sWanted && sContinuous) {
            sMain.post(() -> {
                if (session == sSession && sWanted && !sRunning) listen();
            });
        }
    }

    private static final class Listener implements RecognitionListener {
        @Override public void onReadyForSpeech(Bundle params) {}
        @Override public void onBeginningOfSpeech() {}
        @Override public void onRmsChanged(float rmsdB) {}
        @Override public void onBufferReceived(byte[] buffer) {}
        @Override public void onEndOfSpeech() {}
        @Override public void onEvent(int eventType, Bundle params) {}

        @Override public void onPartialResults(Bundle partialResults) {
            if (sWanted) deliver(partialResults, false);
        }

        @Override public void onResults(Bundle results) {
            final int session = sSession;
            if (sWanted) deliver(results, true);
            utteranceDone(session);
        }

        @Override public void onError(int error) {
            final int session = sSession;
            if (!sWanted) {
                sRunning = false;
                return;
            }
            switch (error) {
                case SpeechRecognizer.ERROR_NO_MATCH:
                case SpeechRecognizer.ERROR_SPEECH_TIMEOUT:
                    nativeResult(new String[0], null, true); // heard nothing usable
                    utteranceDone(session);
                    return;
                case SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS:
                    sRunning = false;
                    nativeError("Microphone permission is off for Starlit Party");
                    return;
                case SpeechRecognizer.ERROR_RECOGNIZER_BUSY:
                    sRunning = false;
                    if (sRecognizer != null) {
                        sRecognizer.destroy();
                        sRecognizer = null;
                    }
                    nativeStopped("recognizer busy");
                    return;
                default:
                    sRunning = false;
                    nativeStopped("recognizer error " + error);
            }
        }
    }
}
