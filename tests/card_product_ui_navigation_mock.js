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
  forceEnabled: false,
  windowPreset: 1,
  failLogging: false,
  hiddenCreateCalls: 0,
  pendingCreation: null,
  creationProgress: "1,0,0,0,0",
  surveyProgress: "2,3,3,2,0,0",
  surveyFound: [1, 3],
  surveyPasswords: [],
  surveyCancels: 0,
  failSurvey: false,
  openedMaintenance: [],
  measuredMaintenance: [],
  endedMaintenance: [],
  maintenanceSpace: "4294967296,1073741824,536870912",
  protectionBlocked: false,
  protectionQueries: [],
  delayProtection: false,
  completeProtection: null,
  dropTargets: [],
  dropClears: 0,
  dropStatus: "0,0,0,0",
  dropCancels: 0,
  filePercent: 0,
    selectFilesCalls: 0,
  failDropTarget: false,
  failOpenMaintenance: false,
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
  const displayContainers = () => {
    const ids = fixture.orderIds || [];
    return [...fixture.containers.filter((item) => !ids.includes(item.id)),
      ...ids.map((id) => fixture.containers.find((item) => item.id === id)).filter(Boolean)];
  };
  const bridge = {
    beginPartitionRefresh: () => { fixture.partitionScans = (fixture.partitionScans || 0) + 1; return true; },
    getPartitionRefreshStatus: () => fixture.partitionPending ? 1 : 2,
    getPartitionDisplay: id => id === 1 ? "T: · 测试数据 <img src=x>" : "C: · 系统",
    getPartitionList: () => fixture.partitionList ?? "1,0,3,1073741824,0\n2,0,1,536870912,1",
    beginPartitionCheck: () => { fixture.partitionChecks = (fixture.partitionChecks || 0) + 1; return true; },
    getPartitionCheckStatus: () => fixture.partitionCheckPending ? 1 : fixture.partitionCheckFails ? 3 : 2,
    selectPartition: id => { fixture.partitionSelected = 0; if (fixture.partitionSelectionFails) throw new Error("stale target"); fixture.partitionSelected = id; return true; },
    clearPartitions: async () => { fixture.partitionSelected = 0; },
    openDiskManagement: () => { if (fixture.partitionToolFails) throw new Error("tool failed"); fixture.partitionTools = (fixture.partitionTools || 0) + 1; return true; },
    getAutoMountEnabled: () => {
      if (fixture.onAutoRead) {
        const callback = fixture.onAutoRead;
        fixture.onAutoRead = null;
        callback();
      }
      if (fixture.failAutoRead) throw new Error("preference unreadable");
      return fixture.autoMount === true;
    },
    setAutoMountEnabled: (value) => {
      if (fixture.failAutoSave) throw new Error("preference write failed");
      fixture.autoMount = value;
    },
    submitLoginPasswordBytes: () => {
      if (fixture.failLogin) throw new Error("login rejected");
      fixture.state = 3;
    },
    reorderContainers: (packed) => {
      if (fixture.failOrder) throw new Error("order write failed");
      const ids = packed.split(",").map(Number);
      fixture.orderIds = ids;
    },
    mountContainer: () => {
      if (!fixture.importNext) return false;
      fixture.containers.unshift(fixture.importNext);
      fixture.importNext = null;
      return true;
    },
    getForceUnmountEnabled: () => fixture.forceEnabled,
    setForceUnmountEnabled: (value) => {
      if (fixture.failForcePreference) throw new Error("preference write failed");
      fixture.forceEnabled = value;
    },
    getStartupView: () => "session",
    getWindowSize: () => fixture.windowPreset,
    beginMountBatch: (ids) => beginBatch(ids, true),
    beginUnmountBatch: (ids) => beginBatch(ids, false),
    beginForceUnmountBatch: (ids) => beginBatch(ids, false, true),
    beginDecoySurvey: (bytes) => {
      if (fixture.failSurvey) throw new Error("survey refused");
      fixture.surveyPasswords.push(Array.from(bytes));
      return true;
    },
    getDecoySurveyProgress: () => fixture.surveyProgress,
    getDecoySurveyId: (index) => fixture.surveyFound[index],
    cancelDecoySurvey: () => {
      fixture.surveyCancels += 1;
      return true;
    },
    beginOpenDecoyMaintenance: (id) => beginVolume(() => {
      if (fixture.failOpenMaintenance) throw new Error("simulated refusal");
      fixture.openedMaintenance.push(id);
      const container = fixture.containers.find(item => item.id === id);
      container.mounted = true;
      container.drive = id === 1 ? "X:" : "Y:";
    }),
    beginMeasureDecoySpace: (id) => beginVolume(() => {
      if (fixture.failMeasureMaintenance) throw new Error("simulated measurement failure");
      fixture.measuredMaintenance.push(id);
    }),
    getDecoyMaintenanceSpace: () => fixture.maintenanceSpace,
    getDecoyMaintenanceWriteBlocked: (id) => {
      fixture.protectionQueries.push(id);
      if (fixture.dropStatus.startsWith("1,")) throw new Error("E_PENDING: upload owns session");
      if (fixture.delayProtection) return new Promise(resolve => { fixture.completeProtection = resolve; });
      return fixture.protectionBlocked;
    },
    setExternalDropTarget: (id, directory) => {
      if (fixture.failDropTarget) throw new Error("the window refused to arm");
      fixture.dropTargets.push(`${id}/${directory}`);
      fixture.dropArmed = true;
      if (!fixture.dropStatus.startsWith("1,")) fixture.dropStatus = "0,0,0,0";
    },
    clearExternalDropTarget: () => { fixture.dropClears += 1; fixture.dropArmed = false; },
    getExternalFileProgress: () => fixture.filePercent,
    getExternalDropStatus: () => fixture.dropStatus,
    selectExternalFiles: () => { fixture.selectFilesCalls += 1; return false; },
    cancelExternalDrop: () => {
      fixture.dropCancels += 1;
      return true;
    },
    beginEndDecoyMaintenance: (id) => beginVolume(() => {
      if (fixture.failEndMaintenance) throw new Error("simulated end failure");
      fixture.endedMaintenance.push(id);
      bridge.unmountContainer(id);
    }),
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
    openConfiguration: () => {
      fixture.configurationCalls = (fixture.configurationCalls || 0) + 1;
      if (fixture.failConfiguration) throw new Error("configuration unavailable");
    },
    hasActiveSession: () => fixture.state === 3,
    canResetCardRecord: () => false,
    canLoginReset: () => fixture.state === 2,
    getRememberContainerPaths: () => true,
    getMountImportedContainers: () => false,
    getContainerCount: () => fixture.containers.length,
    getContainerId: (i) => displayContainers()[i].id,
    getContainerName: (i) => displayContainers()[i].name,
    getContainerPartition: (i) => Boolean(displayContainers()[i].partition),
    getContainerConversion: (i) => displayContainers()[i].conversion || 0,
    getContainerMounted: (i) => displayContainers()[i].mounted,
    getContainerSizeMib: (i) => displayContainers()[i].size,
    getContainerDriveLetter: (i) => displayContainers()[i].drive,
    getOperationCount: () => 0,
    beginCreateContainerBMib: () => {
      fixture.hiddenCreateCalls += 1;
      fixture.creationProgress = "1,0,0,0,0";
      fixture.pendingCreation = {
        resolve: () => { fixture.creationProgress = "2,5,100,90,3"; },
        reject: () => { fixture.creationProgress = "3,3,41,0,3"; },
      };
    },
    beginPartitionConversion: id => {
      if (!id || id !== fixture.partitionSelected) throw new Error("stale conversion consent");
      fixture.partitionConverted = id; fixture.partitionSelected = 0;
      fixture.containers.push({id: 101, name: "转换中的分区", mounted: false, size: 0, drive: "", partition: true, conversion: 1});
      fixture.creationProgress = "1,3,23,0,1";
      fixture.pendingCreation = {
        pause: () => { fixture.containers.find(c=>c.id===101).conversion=2; fixture.creationProgress="6,3,23,0,1"; },
        reject: () => { fixture.creationProgress="3,3,23,0,1"; },
      };
      return true;
    },
    beginPartitionResume: id => {
      const entry=fixture.containers.find(c=>c.id===id);
      if (!entry || entry.conversion!==2) throw new Error("not paused");
      fixture.partitionResumed=id; entry.conversion=1; fixture.creationProgress="1,3,24,0,1";
      fixture.pendingCreation={
        resolve:()=>{entry.conversion=0;entry.mounted=true;entry.size=1024;entry.drive="W:";fixture.creationProgress="2,5,100,87,1";},
        pause:()=>{entry.conversion=2;fixture.creationProgress="6,3,48,0,1";},
        reject:()=>{fixture.creationProgress="3,3,48,0,1";},
      };
      return true;
    },
    beginPartitionCreate: (id) => {
      if (!id || id !== fixture.partitionSelected) throw new Error("stale partition consent");
      fixture.partitionCreated = id;
      fixture.partitionSelected = 0;
      fixture.creationProgress = "1,3,20,0,3";
      fixture.pendingCreation = {
        resolve: () => {
          fixture.containers.push({id: 100, name: "磁盘容器 100", mounted: true, size: 1024, drive: "W:", partition: true});
          fixture.creationProgress = "2,5,100,87,3";
        },
        reject: () => { fixture.creationProgress = "3,3,41,0,3"; },
        cancel: () => { fixture.creationProgress = "4,3,41,0,3"; },
      };
      return true;
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
      fixture.windowPreset = preset;
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
      return (directory === "" ?
        `0\u001e文档\u001f1\u001f0\u001f0\u001f${Date.now() - ((fixture.freshnessDays ?? 1) + 1) * 24 * 60 * 60 * 1000}\u001e报告.xlsx\u001f0\u001f0\u001f188416\u001f${Date.now() - (fixture.freshnessDays ?? 1) * 24 * 60 * 60 * 1000}` :
        "0") + fixture.listingSuffix;
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
