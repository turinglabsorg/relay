import QtQuick
import QtQuick.Controls

CheckBox {
    id: toggle
    property bool dark: true
    spacing: 10
    padding: 0
    implicitHeight: Math.max(24, contentItem.implicitHeight)
    indicator: Rectangle {
        x: 0; y: (toggle.height - height) / 2
        width: 20; height: 20; radius: 5
        color: toggle.checked ? "#b6f3cf" : (toggle.dark ? "#111a16" : "#f7faf8")
        border.color: toggle.activeFocus ? "#6eac88" : (toggle.dark ? "#52675a" : "#9ab2a2")
        Text { anchors.centerIn: parent; text: "✓"; visible: toggle.checked; color: "#183321"; font.pixelSize: 15; font.weight: Font.Bold }
    }
    contentItem: Text {
        text: toggle.text; leftPadding: 30
        color: toggle.dark ? "#e6eeea" : "#243c30"
        font.pixelSize: 13; wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter
    }
}
