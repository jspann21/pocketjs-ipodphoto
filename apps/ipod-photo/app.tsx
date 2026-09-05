import { createSignal, For, Show } from "solid-js";
import { Text, View } from "@pocketjs/framework/components";
import { createWavPlayer, type WavPcm, type WavPlayer } from "@pocketjs/framework/audio";
import { existsSync, readFileSync, writeFileSync } from "@pocketjs/framework/fs";
import { BTN } from "@pocketjs/framework/input";
import { onButtonPress, onFrame } from "@pocketjs/framework/lifecycle";

type Page = "home" | "about" | "sound" | "saved";

const ITEMS = ["About", "Sound", "Saved counter"] as const;
const COUNT_FILE = "saved-counter.txt";

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function makeGentleTone(): WavPcm {
  const sampleRate = 11025;
  const frames = Math.floor(sampleRate * 0.32);
  const data = new Int16Array(frames);
  const halfPeriod = Math.max(1, Math.floor(sampleRate / (523 * 2)));
  for (let start = 0, sample = 900; start < frames; start += halfPeriod) {
    data.fill(sample, start, Math.min(frames, start + halfPeriod));
    sample = -sample;
  }
  const quietFrames = 32;
  data.fill(0, 0, quietFrames);
  data.fill(0, frames - quietFrames);
  return { sampleRate, channels: 1, frames, data };
}

export default function IpodPhotoHome() {
  const [page, setPage] = createSignal<Page>("home");
  const [selected, setSelected] = createSignal(0);
  const [message, setMessage] = createSignal("Choose an item");
  const [count, setCount] = createSignal<number | null>(null);
  let player: WavPlayer | undefined;
  let wasPlaying = false;

  const backToMenu = () => {
    try {
      if (player?.playing()) player.stop();
    } catch {
      // Navigation remains available if a native stream has failed.
    }
    wasPlaying = false;
    setPage("home");
    setMessage("Choose an item");
  };

  const showAbout = () => {
    setPage("about");
    setMessage("PocketJS iPod Photo");
  };

  const playSound = () => {
    setPage("sound");
    setMessage("Playing a gentle tone");
    try {
      if (!player) {
        const nextPlayer = createWavPlayer();
        if (!nextPlayer.loadPcm(makeGentleTone())) {
          try {
            nextPlayer.dispose();
          } catch {
            // Keep the UI alive when a host rejects cleanup after load failure.
          }
          throw new Error("audio is unavailable");
        }
        player = nextPlayer;
      } else {
        try {
          player.stop();
        } catch {
          // A stale native stream must not take down the menu.
        }
      }
      player.setVolume(0.45);
      player.play();
      wasPlaying = true;
    } catch (error) {
      setMessage(`Sound error: ${errorText(error).slice(0, 26)}`);
    }
  };

  const updateSavedCounter = () => {
    setPage("saved");
    try {
      let next = 1;
      if (existsSync(COUNT_FILE)) {
        const stored = Number(readFileSync(COUNT_FILE, "utf8"));
        if (!Number.isSafeInteger(stored) || stored < 0) {
          throw new Error("saved counter is corrupt");
        }
        next = stored + 1;
      }
      writeFileSync(COUNT_FILE, String(next));
      setCount(next);
      setMessage("Saved to iPod storage");
    } catch (error) {
      setCount(null);
      setMessage(`Storage error: ${errorText(error).slice(0, 25)}`);
    }
  };

  const select = () => {
    if (page() === "sound") {
      playSound();
      return;
    }
    if (page() !== "home") return;
    switch (selected()) {
      case 0:
        showAbout();
        break;
      case 1:
        playSound();
        break;
      case 2:
        updateSavedCounter();
        break;
    }
  };

  onButtonPress(BTN.UP | BTN.LEFT, () => {
    if (page() === "home") setSelected((value) => Math.max(0, value - 1));
  });
  onButtonPress(BTN.DOWN | BTN.RIGHT, () => {
    if (page() === "home") setSelected((value) => Math.min(ITEMS.length - 1, value + 1));
  });
  onButtonPress(BTN.CIRCLE, select);
  onButtonPress(BTN.TRIANGLE, backToMenu);

  onFrame(() => {
    try {
      if (player?.playing()) {
        player.pump();
        if (wasPlaying && !player.playing()) {
          wasPlaying = false;
          setMessage("Tone finished");
        }
      }
    } catch (error) {
      try {
        player?.stop();
      } catch {
        // Preserve the visible error even if the native stream is already gone.
      }
      setMessage(`Sound error: ${errorText(error).slice(0, 26)}`);
    }
  });

  return (
    <View class="w-full h-full flex-col bg-[#f1f1f1] overflow-hidden">
      <View class="h-[24] flex-row items-center justify-between px-[8] bg-gradient-to-b from-[#f8f8f8] to-[#bfc1c4] border-[#777777]">
        <Text class="text-sm text-[#111111] font-bold">iPod</Text>
        <Text class="text-xs text-[#333333]">PocketJS</Text>
      </View>

      <View class="flex-1 flex-col bg-[#fafafa]" style={{ display: page() === "home" ? 0 : 1 }}>
          <Text class="h-[22] px-[9] pt-[5] text-xs text-[#555555]">Home</Text>
          <View class="flex-1 flex-col px-[5]">
            <For each={ITEMS}>
              {(item, index) => (
                <View
                  class={index() === selected()
                    ? "h-[29] flex-row items-center justify-between px-[8] bg-gradient-to-b from-[#6db3e8] to-[#1b73ba]"
                    : "h-[29] flex-row items-center justify-between px-[8] bg-[#fafafa]"}
                >
                  <Text class={index() === selected() ? "text-sm text-white font-bold" : "text-sm text-[#202020]"}>{item}</Text>
                  <Text class={index() === selected() ? "text-sm text-white font-bold" : "text-sm text-[#777777]"}>{">"}</Text>
                </View>
              )}
            </For>
          </View>
          <Text class="h-[23] px-[9] pt-[4] text-xs text-[#666666]">Wheel browse · center select</Text>
      </View>

      <View class="flex-1 flex-col items-center justify-center gap-2 bg-[#fafafa] px-[10]" style={{ display: page() === "about" ? 0 : 1 }}>
          <Text class="text-lg text-[#111111] font-bold">PocketJS</Text>
          <Text class="text-sm text-[#333333]">iPod Photo home</Text>
          <Text class="text-xs text-[#666666]">A small app for a classic screen.</Text>
          <Text class="text-xs text-[#666666]">{message()}</Text>
          <Text class="text-xs text-[#555555]">Menu returns home</Text>
      </View>

      <View class="flex-1 flex-col items-center justify-center gap-2 bg-[#fafafa] px-[10]" style={{ display: page() === "sound" ? 0 : 1 }}>
          <Text class="text-lg text-[#111111] font-bold">Sound</Text>
          <Text class="text-sm text-[#333333]">{message()}</Text>
          <Text class="text-xs text-[#666666]">Center plays it again</Text>
          <Text class="text-xs text-[#555555]">Menu returns home</Text>
      </View>

      <View class="flex-1 flex-col items-center justify-center gap-2 bg-[#fafafa] px-[10]" style={{ display: page() === "saved" ? 0 : 1 }}>
          <Text class="text-lg text-[#111111] font-bold">Saved counter</Text>
          <Show when={count() !== null} fallback={<Text class="text-sm text-[#b3261e]">{message()}</Text>}>
            <Text class="text-2xl text-[#1b73ba] font-bold">{count()}</Text>
            <Text class="text-sm text-[#333333]">{message()}</Text>
          </Show>
          <Text class="text-xs text-[#555555]">Menu returns home</Text>
      </View>
    </View>
  );
}
