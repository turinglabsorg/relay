import QtQuick
import QtQuick.Controls

TextField {
    id: field
    property bool dark: true
    implicitHeight: 48
    leftPadding: 15
    rightPadding: 15
    topPadding: 12
    bottomPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    font.pixelSize: 14
    color: dark ? "#e6eeea" : "#243c30"
    placeholderTextColor: dark ? "#71867a" : "#748a7c"
    selectionColor: "#a9f0c7"
    selectedTextColor: "#102219"
    background: Rectangle {
        radius: 10
        color: field.dark ? "#111a16" : "#f7faf8"
        border.width: 1
        border.color: field.activeFocus ? "#a9f0c7" : (field.dark ? "#334239" : "#d2e0d7")
    }
}
