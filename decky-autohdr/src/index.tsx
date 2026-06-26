import {
  ButtonItem,
  Field,
  PanelSection,
  PanelSectionRow,
  SliderField,
  DropdownItem,
  ToggleField,
} from "@decky/ui";
import {
  callable,
  definePlugin,
} from "@decky/api";
import { FC, useEffect, useState } from "react";
import { FaCopy, FaDownload } from "react-icons/fa";

const getGlobalSettings = callable<[], Record<string, unknown>>("get_global_settings");
const updateGlobalSettings = callable<[Record<string, unknown>], Record<string, unknown>>("update_global_settings");
const listAppProfiles = callable<[], Array<{ key: string; displayName: string; steamAppId: string }>>("list_app_profiles");
const listUserPresets = callable<[], Array<{ id: string; displayName: string }>>("list_user_presets");
const applyUserPreset = callable<[string, string | null], Record<string, unknown>>("apply_user_preset");
const installAutohdrVk = callable<[], Record<string, string>>("install_autohdr_vk");
const isAutohdrInstalled = callable<[], boolean>("is_autohdr_installed");
const getLaunchOption = callable<[], string>("get_launch_option");
const steamAppKey = callable<[string], string>("steam_app_key");

const presetOptions = [
  { data: "linear", label: "Linear" },
  { data: "scurve", label: "S-Curve" },
  { data: "scurve_boosted", label: "S-Curve Boosted" },
  { data: "scurve_lifted", label: "S-Curve Lifted" },
  { data: "exponential", label: "Exponential" },
  { data: "custom", label: "Custom" },
  { data: "user", label: "User Preset" },
];

const Content: FC = () => {
  const [settings, setSettings] = useState<Record<string, unknown>>({});
  const [profiles, setProfiles] = useState<Array<{ key: string; displayName: string }>>([]);
  const [presets, setPresets] = useState<Array<{ id: string; displayName: string }>>([]);
  const [installed, setInstalled] = useState(false);
  const [launchOption, setLaunchOption] = useState("~/autohdr %command%");
  const [selectedPreset, setSelectedPreset] = useState("scurve");

  const reload = async () => {
    setSettings(await getGlobalSettings());
    setProfiles(await listAppProfiles());
    setPresets(await listUserPresets());
    setInstalled(await isAutohdrInstalled());
    setLaunchOption(await getLaunchOption());
  };

  useEffect(() => {
    reload();
  }, []);

  const save = async (patch: Record<string, unknown>) => {
    const next = await updateGlobalSettings(patch);
    setSettings(next);
  };

  const copyLaunchOption = async () => {
    if (navigator?.clipboard?.writeText) {
      await navigator.clipboard.writeText(launchOption);
    }
  };

  return (
    <>
      <PanelSection title="Installation">
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={async () => { await installAutohdrVk(); await reload(); }}>
            <FaDownload style={{ display: "inline", paddingRight: "4px" }} />
            {installed ? "Reinstall autohdr-vk Layer" : "Install autohdr-vk Layer"}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={copyLaunchOption}>
            <FaCopy style={{ display: "inline", paddingRight: "4px" }} />
            Copy Launch Option
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <Field label="Launch Option">{launchOption}</Field>
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Global AutoHDR">
        <PanelSectionRow>
          <DropdownItem
            label="Tone Curve Preset"
            selectedOption={String(settings.ToneCurvePreset ?? selectedPreset)}
            onChange={(option) => {
              setSelectedPreset(option.data as string);
              save({ ToneCurvePreset: option.data });
            }}
            rgOptions={presetOptions}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Reference Nits"
            value={Number(settings.ReferenceNits ?? 203)}
            min={80}
            max={480}
            step={1}
            onChange={(value) => save({ ReferenceNits: Math.round(value) })}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Max Nits"
            value={Number(settings.MaxNits ?? 1000)}
            min={200}
            max={4000}
            step={10}
            onChange={(value) => save({ MaxNits: value })}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Gamut Expansion"
            value={Number(settings.GamutExpansion ?? 1.5)}
            min={0}
            max={20}
            step={0.1}
            onChange={(value) => save({ GamutExpansion: value })}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Vibrance"
            value={Number(settings.Vibrance ?? 0)}
            min={0}
            max={10}
            step={0.1}
            onChange={(value) => save({ Vibrance: value })}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField
            label="Black Point"
            value={Number(settings.BlackPoint ?? 0)}
            min={-0.01}
            max={0.01}
            step={0.001}
            onChange={(value) => save({ BlackPoint: value })}
          />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="User Presets (read-only apply)">
        {presets.length === 0 ? (
          <PanelSectionRow>
            <Field label="No presets">Calibrate presets in desktop Plasma mode.</Field>
          </PanelSectionRow>
        ) : (
          presets.map((preset) => (
            <PanelSectionRow key={preset.id}>
              <ButtonItem layout="below" onClick={() => applyUserPreset(preset.id, null).then(reload)}>
                Apply {preset.displayName}
              </ButtonItem>
            </PanelSectionRow>
          ))
        )}
      </PanelSection>

      <PanelSection title="Saved App Profiles">
        {profiles.length === 0 ? (
          <PanelSectionRow>
            <Field label="No profiles">Desktop calibrations appear here automatically.</Field>
          </PanelSectionRow>
        ) : (
          profiles.map((profile) => (
            <PanelSectionRow key={profile.key}>
              <Field label={profile.displayName}>{profile.key}</Field>
            </PanelSectionRow>
          ))
        )}
      </PanelSection>
    </>
  );
};

export default definePlugin(() => ({
  title: <div className="DeckyAutoHDR">AutoHDR</div>,
  content: <Content />,
  icon: <FaDownload />,
}));
