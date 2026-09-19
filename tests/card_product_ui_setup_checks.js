"use strict";
(async () => {
  const $ = (selector) => document.querySelector(selector);
  const wait = async (condition) => {
    for (let i = 0; i < 100; i += 1) {
      if (condition()) return;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error("Timed out waiting for setup state");
  };
  const results = [];
  const check = (condition, message) => {
    if (!condition) throw new Error(message);
    results.push(message);
  };
  const fixture = window.setupFixture;
  const setupInputs = () =>
    ["#setup-user-sn", "#setup-api-key", "#setup-user-pin"].map($);
  const userSn = "0123456789abcdef0123456789ABCDEF";
  const apiKey = "ab".repeat(64);
  const wiped = (entry) =>
    entry.arrays.every((array) => array.every((value) => value === 0));
  const type = (selector, value) => {
    const input = $(selector);
    input.value = value;
    input.dispatchEvent(new window.Event("input", { bubbles: true }));
  };

  try {
    await wait(() => !$("#configuration-surface").hidden);
    check($("#card-session-surface").hidden && $("#authenticated-dashboard").hidden &&
      document.body.dataset.view === "configuration",
      "Setup replaces the session surface on the configuration route");
    check(setupInputs().every((input) => input.type === "text"),
      "Setup shows the supplier values as typed");
    check($("#setup-user-sn").value === "" && $("#setup-api-key").value === "",
      "Setup never prefills a value only the supplier knows");

    check($("#setup-user-pin").value === "11111111",
      "Setup prefills the shipped user password on every route");
    check(!$("#setup-back").hidden && !$("#setup-back").disabled,
      "The way back is offered on every route into setup");

    if (fixture.credentialStatus === 0) {
      check($("#setup-lead").textContent.includes("已保存"),
        "A readable stored set is named as one");
      check($("#setup-status").textContent.includes("消耗一次尝试次数"),
        "Going back on a stored set says what it costs");
      document.title = `PASS: ${results.length} setup checks`;
    } else {
      check($("#setup-save").disabled, "Setup cannot save an incomplete set");

      type("#setup-user-sn", userSn);
      type("#setup-api-key", "00ff");
      check($("#setup-save").disabled && $("#setup-api-key").dataset.valid === "false" &&
        $("#setup-user-sn").dataset.valid === "true",
        "Setup marks the value that is not the right length");
      type("#setup-user-pin", "1234567!");
      check($("#setup-user-pin").dataset.valid === "false",
        "Setup rejects a user password that is not alphanumeric");

      type("#setup-api-key", apiKey);
      type("#setup-user-pin", "11111111");
      check(!$("#setup-save").disabled, "A complete set can be saved");

      fixture.outcome = 2;
      $("#setup-save").click();
      await wait(() => fixture.saved.length === 1 && !$("#setup-save").disabled);
      check($("#setup-status").dataset.tone === "error",
        "A set that could not be read back is not reported as saved");
      check(!$("#setup-back").disabled,
        "A failed save still leaves the way back available");
      check($("#setup-status").textContent.includes("原有配置可能已被覆盖"),
        "A read-back failure does not claim the stored set survived");
      check($("#setup-user-sn").value === userSn,
        "A failed save keeps the typed set so it can be corrected");

      fixture.outcome = 0;
      $("#setup-save").click();
      await wait(() => fixture.saved.length === 2 &&
        $("#setup-status").dataset.tone === "ok");
      check(fixture.saved[1].values[0] === userSn &&
        fixture.saved[1].values[1] === apiKey &&
        fixture.saved[1].values[2] === "11111111",
        "Setup sends the three values in the documented order");
      check(wiped(fixture.saved[0]) && wiped(fixture.saved[1]),
        "Setup wipes every byte array it handed to the bridge");
      check($("#setup-user-sn").value === "" && $("#setup-api-key").value === "" &&
        $("#setup-user-pin").value === "",
        "A saved set is cleared from the page");
      check($("#setup-status").dataset.tone === "ok" && $("#setup-save").disabled,
        "A saved set is reported and cannot be saved again unchanged");
      check($("#setup-status").textContent.includes("消耗一次尝试次数"),
        "Going back says what it costs before the user chooses it");

      fixture.failContinue = true;
      $("#setup-back").click();
      await wait(() => fixture.continueCalls === 1 &&
        $("#setup-status").dataset.tone === "error");
      check(!$("#setup-back").disabled,
        "A refused transition leaves setup usable");

      fixture.failContinue = false;
      $("#setup-back").click();
      await wait(() => fixture.continueCalls === 2);
      check($("#setup-back").disabled,
        "An accepted transition stops the retired page from asking again");
      document.title = `PASS: ${results.length} setup checks`;
    }
  } catch (error) {
    document.title = `FAIL: ${error.message}`;
    results.push(`FAIL: ${error.stack}`);
  }
  const output = document.createElement("pre");
  output.id = "setup-test-results";
  output.textContent = document.title + "\n" + results.join("\n");
  document.body.prepend(output);
})();
