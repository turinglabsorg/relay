import QtQuick
import QtQuick.Controls

Button {
    id: button
    property string label: ""
    property string iconName: ""
    property bool accent: false
    property bool compact: false
    property bool dark: true
    property string hint: ""
    property color accentColor: "#b6f3cf"
    implicitWidth: compact ? 42 : Math.max(108, caption.implicitWidth + (iconName ? 30 : 0) + 36)
    implicitHeight: compact ? 42 : 48
    padding: compact ? 10 : 18
    opacity: enabled ? 1 : 0.45
    hoverEnabled: true
    Accessible.name: label || hint
    contentItem: Item {
        Row {
            anchors.centerIn: parent
            spacing: button.iconName && button.label ? 10 : 0
            Icon {
                visible: button.iconName !== ""
                name: button.iconName
                ink: button.accent ? "#13251d" : (button.dark ? "#d6dfdc" : "#33453e")
                width: visible ? 20 : 0; height: 20
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: caption
                visible: button.label !== ""
                text: button.label
                width: Math.min(implicitWidth, Math.max(0, button.availableWidth - (button.iconName ? 30 : 0)))
                elide: Text.ElideRight
                color: button.accent ? "#13251d" : (button.dark ? "#e0e8e5" : "#263c31")
                font.pixelSize: 14; font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
    background: Rectangle {
        radius: 11
        color: button.accent ? (button.hovered ? "#d3ffdf" : button.accentColor) : (button.hovered ? (button.dark ? "#25332f" : "#e4eee8") : (button.dark ? "#19231f" : "#f2f6f3"))
        border.width: button.accent && !button.activeFocus ? 0 : 1
        border.color: button.activeFocus ? "#6eac88" : (button.dark ? "#34413a" : "#d4dfd8")
        Behavior on color { ColorAnimation { duration: 130 } }
    }
    ToolTip.visible: hovered && hint !== ""
    ToolTip.text: hint
    ToolTip.delay: 550
}
