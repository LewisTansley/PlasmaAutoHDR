var decky_plugin = (function (React, _manifest) {
  'use strict';

  var jsxRuntime = {exports: {}};

  var reactJsxRuntime_production_min = {};

  /**
   * @license React
   * react-jsx-runtime.production.min.js
   *
   * Copyright (c) Facebook, Inc. and its affiliates.
   *
   * This source code is licensed under the MIT license found in the
   * LICENSE file in the root directory of this source tree.
   */
  var f=React,k=Symbol.for("react.element"),l=Symbol.for("react.fragment"),m=Object.prototype.hasOwnProperty,n=f.__SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED.ReactCurrentOwner,p={key:true,ref:true,__self:true,__source:true};
  function q(c,a,g){var b,d={},e=null,h=null;void 0!==g&&(e=""+g);void 0!==a.key&&(e=""+a.key);void 0!==a.ref&&(h=a.ref);for(b in a)m.call(a,b)&&!p.hasOwnProperty(b)&&(d[b]=a[b]);if(c&&c.defaultProps)for(b in a=c.defaultProps,a) void 0===d[b]&&(d[b]=a[b]);return {$$typeof:k,type:c,key:e,ref:h,props:d,_owner:n.current}}reactJsxRuntime_production_min.Fragment=l;reactJsxRuntime_production_min.jsx=q;reactJsxRuntime_production_min.jsxs=q;

  {
    jsxRuntime.exports = reactJsxRuntime_production_min;
  }

  var jsxRuntimeExports = jsxRuntime.exports;

  const bgStyle1 = 'background: #16a085; color: black;';
  const log = (name, ...args) => {
      console.log(`%c @decky/ui %c ${name} %c`, bgStyle1, 'background: #1abc9c; color: black;', 'background: transparent;', ...args);
  };
  const group = (name, ...args) => {
      console.group(`%c @decky/ui %c ${name} %c`, bgStyle1, 'background: #1abc9c; color: black;', 'background: transparent;', ...args);
  };
  const groupEnd = (name, ...args) => {
      console.groupEnd();
      if (args?.length > 0)
          console.log(`^ %c @decky/ui %c ${name} %c`, bgStyle1, 'background: #1abc9c; color: black;', 'background: transparent;', ...args);
  };
  const debug = (name, ...args) => {
      console.debug(`%c @decky/ui %c ${name} %c`, bgStyle1, 'background: #1abc9c; color: black;', 'color: blue;', ...args);
  };
  const warn = (name, ...args) => {
      console.warn(`%c @decky/ui %c ${name} %c`, bgStyle1, 'background: #ffbb00; color: black;', 'color: blue;', ...args);
  };
  const error = (name, ...args) => {
      console.error(`%c @decky/ui %c ${name} %c`, bgStyle1, 'background: #FF0000;', 'background: transparent;', ...args);
  };
  class Logger {
      constructor(name) {
          this.name = name;
          this.name = name;
      }
      log(...args) {
          log(this.name, ...args);
      }
      debug(...args) {
          debug(this.name, ...args);
      }
      warn(...args) {
          warn(this.name, ...args);
      }
      error(...args) {
          error(this.name, ...args);
      }
      group(...args) {
          group(this.name, ...args);
      }
      groupEnd(...args) {
          groupEnd(this.name, ...args);
      }
  }

  const logger = new Logger('Webpack');
  let modules = [];
  function initModuleCache() {
      const startTime = performance.now();
      logger.group('Webpack Module Init');
      const id = Math.random();
      let webpackRequire;
      window.webpackChunksteamui.push([
          [id],
          {},
          (r) => {
              webpackRequire = r;
          },
      ]);
      logger.log('Initializing all modules. Errors here likely do not matter, as they are usually just failing module side effects.');
      for (let i of Object.keys(webpackRequire.m)) {
          try {
              const module = webpackRequire(i);
              if (module) {
                  modules.push(module);
              }
          }
          catch (e) {
              logger.debug('Ignoring require error for module', i, e);
          }
      }
      logger.groupEnd(`Modules initialized in ${performance.now() - startTime}ms...`);
  }
  initModuleCache();
  const findModuleDetailsByExport = (filter, minExports) => {
      for (const m of modules) {
          if (!m)
              continue;
          for (const mod of [m.default, m]) {
              if (typeof mod !== 'object')
                  continue;
              for (let exportName in mod) {
                  if (mod?.[exportName]) {
                      const filterRes = filter(mod[exportName], exportName);
                      if (filterRes) {
                          return [mod, mod[exportName], exportName];
                      }
                      else {
                          continue;
                      }
                  }
              }
          }
      }
      return [undefined, undefined, undefined];
  };
  const findModuleByExport = (filter, minExports) => {
      return findModuleDetailsByExport(filter)?.[0];
  };
  const findModuleExport = (filter, minExports) => {
      return findModuleDetailsByExport(filter)?.[1];
  };
  const CommonUIModule = modules.find((m) => {
      if (typeof m !== 'object')
          return false;
      for (let prop in m) {
          if (m[prop]?.contextType?._currentValue && Object.keys(m).length > 60)
              return true;
      }
      return false;
  });
  findModuleByExport((e) => e?.toString && /Spinner\)}\)?,.\.createElement\(\"path\",{d:\"M18 /.test(e.toString()));
  findModuleByExport((e) => e.computeRootMatch);

  (undefined && undefined.__setFunctionName) || function (f, name, prefix) {
      if (typeof name === "symbol") name = name.description ? "[".concat(name.description, "]") : "";
      return Object.defineProperty(f, "name", { configurable: true, value: prefix ? "".concat(prefix, " ", name) : name });
  };
  function createPropListRegex(propList, fromStart = true) {
      let regexString = fromStart ? "const\{" : "";
      propList.forEach((prop, propIdx) => {
          regexString += `"?${prop}"?:[a-zA-Z_$]{1,2}`;
          if (propIdx < propList.length - 1) {
              regexString += ",";
          }
      });
      return new RegExp(regexString);
  }

  const buttonItemRegex = createPropListRegex(["highlightOnFocus", "childrenContainerWidth"], false);
  const ButtonItem = Object.values(CommonUIModule).find((mod) => (mod?.render?.toString && buttonItemRegex.test(mod.render.toString())) ||
      mod?.render?.toString?.().includes('childrenContainerWidth:"min"'));

  Object.values(CommonUIModule).find((mod) => mod?.prototype?.SetSelectedOption && mod?.prototype?.BuildMenu);
  const dropdownItemRegex = createPropListRegex(["dropDownControlRef", "description"], false);
  const DropdownItem = Object.values(CommonUIModule).find((mod) => mod?.toString && dropdownItemRegex.test(mod.toString()));

  const Field = findModuleExport((e) => e?.render?.toString().includes('"shift-children-below"'));

  const [mod, panelSection] = findModuleDetailsByExport((e) => e.toString()?.includes('.PanelSection'));
  const PanelSection = panelSection;
  const PanelSectionRow = Object.values(mod).filter((exp) => !exp?.toString()?.includes('.PanelSection'))[0];

  const SliderField = Object.values(CommonUIModule).find((mod) => mod?.toString()?.includes('SliderField,fallback'));

  const manifest = _manifest;
  const API_VERSION = 2;
  if (!manifest?.name) {
      throw new Error('[@decky/api]: Failed to find plugin manifest.');
  }
  const internalAPIConnection = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
  if (!internalAPIConnection) {
      throw new Error('[@decky/api]: Failed to connect to the loader as as the loader API was not initialized. This is likely a bug in Decky Loader.');
  }
  let api;
  try {
      api = internalAPIConnection.connect(API_VERSION, manifest.name);
  }
  catch {
      api = internalAPIConnection.connect(1, manifest.name);
      console.warn(`[@decky/api] Requested API version ${API_VERSION} but the running loader only supports version 1. Some features may not work.`);
  }
  if (api._version != API_VERSION) {
      console.warn(`[@decky/api] Requested API version ${API_VERSION} but the running loader only supports version ${api._version}. Some features may not work.`);
  }
  api.call;
  const callable = api.callable;
  api.addEventListener;
  api.removeEventListener;
  api.routerHook;
  api.toaster;
  api.openFilePicker;
  api.executeInTab;
  api.injectCssIntoTab;
  api.removeCssFromTab;
  api.fetchNoCors;
  api.getExternalResourceURL;
  api.useQuickAccessVisible;
  const definePlugin = (fn) => {
      return (...args) => {
          return fn(...args);
      };
  };

  var DefaultContext = {
    color: undefined,
    size: undefined,
    className: undefined,
    style: undefined,
    attr: undefined
  };
  var IconContext = React.createContext && /*#__PURE__*/React.createContext(DefaultContext);

  var _excluded = ["attr", "size", "title"];
  function _objectWithoutProperties(e, t) { if (null == e) return {}; var o, r, i = _objectWithoutPropertiesLoose(e, t); if (Object.getOwnPropertySymbols) { var n = Object.getOwnPropertySymbols(e); for (r = 0; r < n.length; r++) o = n[r], -1 === t.indexOf(o) && {}.propertyIsEnumerable.call(e, o) && (i[o] = e[o]); } return i; }
  function _objectWithoutPropertiesLoose(r, e) { if (null == r) return {}; var t = {}; for (var n in r) if ({}.hasOwnProperty.call(r, n)) { if (-1 !== e.indexOf(n)) continue; t[n] = r[n]; } return t; }
  function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
  function ownKeys(e, r) { var t = Object.keys(e); if (Object.getOwnPropertySymbols) { var o = Object.getOwnPropertySymbols(e); r && (o = o.filter(function (r) { return Object.getOwnPropertyDescriptor(e, r).enumerable; })), t.push.apply(t, o); } return t; }
  function _objectSpread(e) { for (var r = 1; r < arguments.length; r++) { var t = null != arguments[r] ? arguments[r] : {}; r % 2 ? ownKeys(Object(t), true).forEach(function (r) { _defineProperty(e, r, t[r]); }) : Object.getOwnPropertyDescriptors ? Object.defineProperties(e, Object.getOwnPropertyDescriptors(t)) : ownKeys(Object(t)).forEach(function (r) { Object.defineProperty(e, r, Object.getOwnPropertyDescriptor(t, r)); }); } return e; }
  function _defineProperty(e, r, t) { return (r = _toPropertyKey(r)) in e ? Object.defineProperty(e, r, { value: t, enumerable: true, configurable: true, writable: true }) : e[r] = t, e; }
  function _toPropertyKey(t) { var i = _toPrimitive(t, "string"); return "symbol" == typeof i ? i : i + ""; }
  function _toPrimitive(t, r) { if ("object" != typeof t || !t) return t; var e = t[Symbol.toPrimitive]; if (void 0 !== e) { var i = e.call(t, r); if ("object" != typeof i) return i; throw new TypeError("@@toPrimitive must return a primitive value."); } return ("string" === r ? String : Number)(t); }
  function Tree2Element(tree) {
    return tree && tree.map((node, i) => /*#__PURE__*/React.createElement(node.tag, _objectSpread({
      key: i
    }, node.attr), Tree2Element(node.child)));
  }
  function GenIcon(data) {
    return props => /*#__PURE__*/React.createElement(IconBase, _extends({
      attr: _objectSpread({}, data.attr)
    }, props), Tree2Element(data.child));
  }
  function IconBase(props) {
    var elem = conf => {
      var {
          attr,
          size,
          title
        } = props,
        svgProps = _objectWithoutProperties(props, _excluded);
      var computedSize = size || conf.size || "1em";
      var className;
      if (conf.className) className = conf.className;
      if (props.className) className = (className ? className + " " : "") + props.className;
      return /*#__PURE__*/React.createElement("svg", _extends({
        stroke: "currentColor",
        fill: "currentColor",
        strokeWidth: "0"
      }, conf.attr, attr, svgProps, {
        className: className,
        style: _objectSpread(_objectSpread({
          color: props.color || conf.color
        }, conf.style), props.style),
        height: computedSize,
        width: computedSize,
        xmlns: "http://www.w3.org/2000/svg"
      }), title && /*#__PURE__*/React.createElement("title", null, title), props.children);
    };
    return IconContext !== undefined ? /*#__PURE__*/React.createElement(IconContext.Consumer, null, conf => elem(conf)) : elem(DefaultContext);
  }

  // THIS FILE IS AUTO GENERATED
  function FaDownload (props) {
    return GenIcon({"attr":{"viewBox":"0 0 512 512"},"child":[{"tag":"path","attr":{"d":"M216 0h80c13.3 0 24 10.7 24 24v168h87.7c17.8 0 26.7 21.5 14.1 34.1L269.7 378.3c-7.5 7.5-19.8 7.5-27.3 0L90.1 226.1c-12.6-12.6-3.7-34.1 14.1-34.1H192V24c0-13.3 10.7-24 24-24zm296 376v112c0 13.3-10.7 24-24 24H24c-13.3 0-24-10.7-24-24V376c0-13.3 10.7-24 24-24h146.7l49 49c20.1 20.1 52.5 20.1 72.6 0l49-49H488c13.3 0 24 10.7 24 24zm-124 88c0-11-9-20-20-20s-20 9-20 20 9 20 20 20 20-9 20-20zm64 0c0-11-9-20-20-20s-20 9-20 20 9 20 20 20 20-9 20-20z"},"child":[]}]})(props);
  }function FaCopy (props) {
    return GenIcon({"attr":{"viewBox":"0 0 448 512"},"child":[{"tag":"path","attr":{"d":"M320 448v40c0 13.255-10.745 24-24 24H24c-13.255 0-24-10.745-24-24V120c0-13.255 10.745-24 24-24h72v296c0 30.879 25.121 56 56 56h168zm0-344V0H152c-13.255 0-24 10.745-24 24v368c0 13.255 10.745 24 24 24h272c13.255 0 24-10.745 24-24V128H344c-13.2 0-24-10.8-24-24zm120.971-31.029L375.029 7.029A24 24 0 0 0 358.059 0H352v96h96v-6.059a24 24 0 0 0-7.029-16.97z"},"child":[]}]})(props);
  }

  const getGlobalSettings = callable("get_global_settings");
  const updateGlobalSettings = callable("update_global_settings");
  const listAppProfiles = callable("list_app_profiles");
  const listUserPresets = callable("list_user_presets");
  const applyUserPreset = callable("apply_user_preset");
  const installAutohdrVk = callable("install_autohdr_vk");
  const isAutohdrInstalled = callable("is_autohdr_installed");
  const getLaunchOption = callable("get_launch_option");
  callable("steam_app_key");
  const presetOptions = [
      { data: "linear", label: "Linear" },
      { data: "scurve", label: "S-Curve" },
      { data: "scurve_boosted", label: "S-Curve Boosted" },
      { data: "scurve_lifted", label: "S-Curve Lifted" },
      { data: "exponential", label: "Exponential" },
      { data: "custom", label: "Custom" },
      { data: "user", label: "User Preset" },
  ];
  const Content = () => {
      const [settings, setSettings] = React.useState({});
      const [profiles, setProfiles] = React.useState([]);
      const [presets, setPresets] = React.useState([]);
      const [installed, setInstalled] = React.useState(false);
      const [launchOption, setLaunchOption] = React.useState("~/autohdr %command%");
      const [selectedPreset, setSelectedPreset] = React.useState("scurve");
      const reload = async () => {
          setSettings(await getGlobalSettings());
          setProfiles(await listAppProfiles());
          setPresets(await listUserPresets());
          setInstalled(await isAutohdrInstalled());
          setLaunchOption(await getLaunchOption());
      };
      React.useEffect(() => {
          reload();
      }, []);
      const save = async (patch) => {
          const next = await updateGlobalSettings(patch);
          setSettings(next);
      };
      const copyLaunchOption = async () => {
          if (navigator?.clipboard?.writeText) {
              await navigator.clipboard.writeText(launchOption);
          }
      };
      return (jsxRuntimeExports.jsxs(jsxRuntimeExports.Fragment, { children: [jsxRuntimeExports.jsxs(PanelSection, { title: "Installation", children: [jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsxs(ButtonItem, { layout: "below", onClick: async () => { await installAutohdrVk(); await reload(); }, children: [jsxRuntimeExports.jsx(FaDownload, { style: { display: "inline", paddingRight: "4px" } }), installed ? "Reinstall autohdr-vk Layer" : "Install autohdr-vk Layer"] }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsxs(ButtonItem, { layout: "below", onClick: copyLaunchOption, children: [jsxRuntimeExports.jsx(FaCopy, { style: { display: "inline", paddingRight: "4px" } }), "Copy Launch Option"] }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(Field, { label: "Launch Option", children: launchOption }) })] }), jsxRuntimeExports.jsxs(PanelSection, { title: "Global AutoHDR", children: [jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(DropdownItem, { label: "Tone Curve Preset", selectedOption: String(settings.ToneCurvePreset ?? selectedPreset), onChange: (option) => {
                                  setSelectedPreset(option.data);
                                  save({ ToneCurvePreset: option.data });
                              }, rgOptions: presetOptions }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(SliderField, { label: "Reference Nits", value: Number(settings.ReferenceNits ?? 203), min: 80, max: 480, step: 1, onChange: (value) => save({ ReferenceNits: Math.round(value) }) }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(SliderField, { label: "Max Nits", value: Number(settings.MaxNits ?? 1000), min: 200, max: 4000, step: 10, onChange: (value) => save({ MaxNits: value }) }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(SliderField, { label: "Gamut Expansion", value: Number(settings.GamutExpansion ?? 1.5), min: 0, max: 20, step: 0.1, onChange: (value) => save({ GamutExpansion: value }) }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(SliderField, { label: "Vibrance", value: Number(settings.Vibrance ?? 0), min: 0, max: 10, step: 0.1, onChange: (value) => save({ Vibrance: value }) }) }), jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(SliderField, { label: "Black Point", value: Number(settings.BlackPoint ?? 0), min: -0.01, max: 0.01, step: 0.001, onChange: (value) => save({ BlackPoint: value }) }) })] }), jsxRuntimeExports.jsx(PanelSection, { title: "User Presets (read-only apply)", children: presets.length === 0 ? (jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(Field, { label: "No presets", children: "Calibrate presets in desktop Plasma mode." }) })) : (presets.map((preset) => (jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsxs(ButtonItem, { layout: "below", onClick: () => applyUserPreset(preset.id, null).then(reload), children: ["Apply ", preset.displayName] }) }, preset.id)))) }), jsxRuntimeExports.jsx(PanelSection, { title: "Saved App Profiles", children: profiles.length === 0 ? (jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(Field, { label: "No profiles", children: "Desktop calibrations appear here automatically." }) })) : (profiles.map((profile) => (jsxRuntimeExports.jsx(PanelSectionRow, { children: jsxRuntimeExports.jsx(Field, { label: profile.displayName, children: profile.key }) }, profile.key)))) })] }));
  };
  var index = definePlugin(() => ({
      title: jsxRuntimeExports.jsx("div", { className: "DeckyAutoHDR", children: "AutoHDR" }),
      content: jsxRuntimeExports.jsx(Content, {}),
      icon: jsxRuntimeExports.jsx(FaDownload, {}),
  }));

  return index;

})(React, _manifest);
//# sourceMappingURL=index.js.map
