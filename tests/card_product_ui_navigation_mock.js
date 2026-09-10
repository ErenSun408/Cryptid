"use strict";
window.navigationFixture = {
  state: 3,
  calls: [],
  pending: null,
  delayListing: false,
  listingSuffix: "",
  failUnmount: false,
  chosenLocation: "D:\\Export",
  pickerCalls: 0,
  windowSizes: [],
  failWindowSize: false,
  loggingEnabled: true,
  failLogging: false,
  containers: [
    { id: 1, name: "工作资料", mounted: true, size: 65536, drive: "X:" },
    { id: 2, name: "影像存档", mounted: false, size: 0, drive: "" },
    { id: 3, name: "归档 2019", mounted: true, size: 131072, drive: "Y:" },
  ],
};
{
  const fixture = window.navigationFixture;
  const bridge = {
    getSessionState: () => fixture.state,
    hasActiveSession: () => fixture.state === 3,
    canResetCardRecord: () => false,
    getRememberContainerPaths: () => true,
    getMountImportedContainers: () => false,
    getContainerCount: () => fixture.containers.length,
    getContainerId: (i) => fixture.containers[i].id,
    getContainerName: (i) => fixture.containers[i].name,
    getContainerMounted: (i) => fixture.containers[i].mounted,
    getContainerSizeMib: (i) => fixture.containers[i].size,
    getContainerDriveLetter: (i) => fixture.containers[i].drive,
    getOperationCount: () => 0,
    getOperationLoggingEnabled: () => fixture.loggingEnabled,
    setOperationLoggingEnabled: (enabled) => {
      if (fixture.failLogging) throw new Error("simulated preference failure");
      fixture.loggingEnabled = enabled;
    },
    setWindowSize: (preset) => {
      if (fixture.failWindowSize) throw new Error("simulated resize failure");
      fixture.windowSizes.push(preset);
    },
    chooseContainerDirectory: () => {
      fixture.pickerCalls += 1;
      return fixture.chosenLocation;
    },
    mountAdoptedContainer: (id) => {
      const container = fixture.containers.find((item) => item.id === id);
      container.mounted = true;
      container.drive = "Z:";
      container.size = 1024;
    },
    unmountContainer: (id) => {
      if (fixture.failUnmount) throw new Error("simulated refusal");
      const container = fixture.containers.find((item) => item.id === id);
      container.mounted = false;
      container.drive = "";
      container.size = 0;
    },
    listContainerDirectory: (id, directory) => {
      fixture.calls.push([id, directory]);
      if (fixture.delayListing) {
        return new Promise((resolve) => { fixture.pending = resolve; });
      }
      return (directory === "" ? "0\u001e文档\u001f1\u001f0\u001f0\u001f0\u001e报告.xlsx\u001f0\u001f0\u001f188416\u001f0" : "0") + fixture.listingSuffix;
    },
  };
  window.chrome = window.chrome || {};
  window.chrome.webview = { hostObjects: { nativeBridge: bridge } };
}
