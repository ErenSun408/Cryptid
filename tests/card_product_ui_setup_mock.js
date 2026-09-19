"use strict";
// Stands in for the setup bridge the native host installs when the product
// opens on the configuration surface. It offers exactly the members that
// bridge has, so a page reaching for a product member fails here the way it
// would in the product.
window.setupFixture = {
  startupView: "configuration",
  credentialStatus: window.setupFixtureStatus === undefined
    ? 1
    : window.setupFixtureStatus,
  failStatus: false,
  outcome: 0,
  failSave: false,
  failContinue: false,
  saved: [],
  continueCalls: 0,
};
{
  const fixture = window.setupFixture;
  const bridge = {
    getStartupView: () => Promise.resolve(fixture.startupView),
    getCredentialStatus: () => fixture.failStatus
      ? Promise.reject(new Error("credential status unavailable"))
      : Promise.resolve(fixture.credentialStatus),
    saveCredentialBytes: (userSn, apiKey, userPin) => {
      // What arrived is recorded as text, while the arrays themselves are kept
      // by reference: a check then reads the values the bridge was given and
      // still proves the page wiped the copies it made.
      const read = (values) =>
        values.map((value) => String.fromCharCode(value)).join("");
      fixture.saved.push({
        arrays: [userSn, apiKey, userPin],
        values: [read(userSn), read(apiKey), read(userPin)],
      });
      return fixture.failSave
        ? Promise.reject(new Error("save failed"))
        : Promise.resolve(fixture.outcome);
    },
    continueToSession: () => {
      fixture.continueCalls += 1;
      return fixture.failContinue
        ? Promise.reject(new Error("continue refused"))
        : Promise.resolve(true);
    },
  };
  window.chrome = { webview: { hostObjects: { nativeBridge: bridge } } };
}
