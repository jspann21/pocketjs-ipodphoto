import { For, Show, createSignal } from "solid-js";
import { Text, View } from "@pocketjs/framework/components";
import { BTN } from "@pocketjs/framework/input";
import { onButtonPress } from "@pocketjs/framework/lifecycle";

interface LauncherBridge {
  selected: number;
}

interface IpodHostUi {
  __ipodApps?: string[];
  __ipodLauncher?: LauncherBridge;
}

export default function IpodPhotoLauncher() {
  const host = (globalThis as typeof globalThis & { ui?: IpodHostUi }).ui;
  const apps = host?.__ipodApps ?? [];
  const bridge = host?.__ipodLauncher;
  const [selected, setSelected] = createSignal(0);

  onButtonPress(BTN.LEFT | BTN.RIGHT, (pressed) => {
    if (apps.length === 0) return;
    const direction = pressed & BTN.RIGHT ? 1 : -1;
    setSelected((current) => (current + direction + apps.length) % apps.length);
  });
  onButtonPress(BTN.CIRCLE, () => {
    if (bridge && apps.length !== 0) bridge.selected = selected();
  });

  return (
    <View class="w-full h-full flex-col bg-[#f1f1f1] overflow-hidden">
      <View class="h-[24] flex-row items-center justify-between px-[8] bg-gradient-to-b from-[#f8f8f8] to-[#bfc1c4] border-[#777777]">
        <Text class="text-sm text-[#111111] font-bold">iPod</Text>
        <Text class="text-xs text-[#333333]">PocketJS</Text>
      </View>

      <View class="flex-1 flex-col bg-[#fafafa]">
        <Text class="h-[22] px-[9] pt-[5] text-xs text-[#555555]">Apps</Text>
        <View class="flex-1 flex-col px-[5]">
          <Show when={apps.length > 0} fallback={<Text class="h-[20] px-[8] pt-[2] text-sm text-[#555555]">No apps installed</Text>}>
            <For each={apps}>
          {(name, index) => (
                <View
                  class={index() === selected()
                    ? "h-[18] flex-row items-center justify-between px-[8] bg-gradient-to-b from-[#6db3e8] to-[#1b73ba]"
                    : "h-[18] flex-row items-center justify-between px-[8] bg-[#fafafa]"}
                >
                  <Text class={index() === selected() ? "text-xs text-white font-bold" : "text-xs text-[#202020]"}>{name}</Text>
                  <Text class={index() === selected() ? "text-xs text-white font-bold" : "text-xs text-[#777777]"}>{">"}</Text>
                </View>
          )}
            </For>
          </Show>
        </View>
      </View>

      <Text class="h-[22] px-[9] pt-[4] text-xs text-[#666666]">Wheel browse · center open</Text>
    </View>
  );
}
