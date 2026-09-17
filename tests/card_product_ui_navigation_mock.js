"use strict";
window.navigationFixture = {
  state: 3,
  calls: [],
  pending: null,
  delayListing: false,
  listingSuffix: "",
  failUnmount: false,
  failMount: false,
  chosenLocation: "D:\\Export",
  pickerCalls: 0,
  windowSizes: [],
  failWindowSize: false,
  loggingEnabled: true,
  failLogging: false,
  hiddenCreateCalls: 0,
  pendingCreation: null,
  creationProgress: "1,0,0,0,0",
  acceptCancellation: true,
  failCancellation: false,
  containers: [
    { id: 1, name: "工作资料", mounted: true, size: 65536, drive: "X:" },
    { id: 2, name: "影像存档", mounted: false, size: 0, drive: "" },
    { id: 3, name: "归档 2019", mounted: true, size: 131072, drive: "Y:" },
  ],
};
{
  const fixture = window.navigationFixture;
  const bridge = {
    beginMountBatch: (ids) => beginBatch(ids, true),
    beginUnmountBatch: (ids) => beginBatch(ids, false),
    beginForceUnmountBatch: (ids) => beginBatch(ids, false, true),
    getBatchProgress: () => fixture.batchProgress,
    getBatchItemName: (index) => fixture.batchItems[index].name,
    getBatchItemId: (index) => fixture.batchItems[index].id,
    getBatchItemOutcome: (index) => fixture.batchItems[index].outcome,
    beginMountAdoptedContainer: (id) => beginVolume(() => bridge.mountAdoptedContainer(id)),
    beginUnmountContainer: (id) => {
      fixture.forceTarget = id;
      return beginVolume(() => bridge.unmountContainer(id));
    },
    beginUnmount: () => beginVolume(() => {
      fixture.forceTarget = 0;
      if (fixture.failUnmount) throw new Error("simulated refusal");
      for (const container of fixture.containers) bridge.unmountContainer(container.id);
    }),
    // Like the native side: no identifier crosses the bridge, and the run
    // stops at the first refusal instead of opening the rest.
    beginMountAll: () => beginVolume(() => {
      fixture.forceTarget = 0;
      for (const container of fixture.containers) {
        if (!container.mounted) bridge.mountAdoptedContainer(container.id);
      }
    }),
    beginForceUnmountLast: () => {
      fixture.forceCalls = (fixture.forceCalls || 0) + 1;
      return beginVolume(() => {
        if (fixture.failForce) throw new Error("simulated force failure");
        for (const container of fixture.containers) {
          if (fixture.forceTarget && fixture.forceTarget !== container.id) continue;
          container.mounted = false;
          container.drive = "";
          container.size = 0;
        }
      });
    },
    getVolumeOperationStatus: () => fixture.volumeStatus,
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
    beginCreateContainerBMib: () => {
      fixture.hiddenCreateCalls += 1;
      fixture.creationProgress = "1,0,0,0,0";
      fixture.pendingCreation = {
        resolve: () => { fixture.creationProgress = "2,5,100,90,3"; },
        reject: () => { fixture.creationProgress = "3,3,41,0,3"; },
      };
    },
    beginCreateContainerMib: () => {
      fixture.creationProgress = "1,0,0,0,0";
      fixture.pendingCreation = {
        resolve: () => { fixture.creationProgress = "2,5,100,90,3"; },
        reject: () => { fixture.creationProgress = "3,3,41,0,3"; },
      };
    },
    getCreationProgress: () => fixture.creationProgress,
    cancelContainerCreation: () => {
      if (fixture.failCancellation) throw new Error("simulated cancel request failure");
      return fixture.acceptCancellation;
    },
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
      if (fixture.failMount) throw new Error("simulated mount refusal");
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
  const beginBatch = (ids, mounting, force = false) => {
    fixture.batchRequests = fixture.batchRequests || [];
    fixture.batchRequests.push([mounting, ids, force]);
    fixture.batchItems = ids.split(",").map((id) => ({
      ...fixture.containers.find((item) => item.id === Number(id)), outcome: 0,
    }));
    const items = fixture.batchItems;
    fixture.batchProgress = `1,${mounting ? 0 : 1},${items.length},0,1,${items[0].id}`;
    const complete = () => {
      for (const item of items) {
        if (item.mounted === mounting) { item.outcome = 11; continue; }
        try {
          if (fixture.batchFailId === item.id) throw new Error("simulated failure");
          if (mounting) bridge.mountAdoptedContainer(item.id);
          else if (force) {
            const target = fixture.containers.find((entry) => entry.id === item.id);
            target.mounted = false; target.drive = ""; target.size = 0;
          }
          else bridge.unmountContainer(item.id);
          item.outcome = 2;
        } catch (_) { item.outcome = 4; }
      }
      fixture.batchProgress = `2,${mounting ? 0 : 1},${items.length},${items.length},0,0`;
    };
    if (fixture.delayVolume) fixture.completeVolume = complete;
    else complete();
  };
  const beginVolume = (operation) => {
    fixture.volumeStarts = (fixture.volumeStarts || 0) + 1;
    fixture.volumeStatus = 1;
    const complete = () => {
      try { operation(); fixture.volumeStatus = 2; }
      catch (_) { fixture.volumeStatus = fixture.volumeFailureStatus || 4; }
    };
    if (fixture.delayVolume) fixture.completeVolume = complete;
    else complete();
    return true;
  };
  window.chrome.webview = { hostObjects: { nativeBridge: bridge } };
}
