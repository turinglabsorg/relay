import QtQuick

Item {
    id: art
    property bool dark: true
    clip: true
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width * .95, parent.height)
        height: width
        radius: width / 2
        opacity: .5
        gradient: Gradient {
            GradientStop { position: 0; color: art.dark ? "#243c32" : "#ceeade" }
            GradientStop { position: 1; color: art.dark ? "#16231d" : "#edf7f0" }
        }
    }
    Canvas {
        anchors.fill: parent
        onPaint: {
            let c=getContext("2d"); c.reset()
            let s=Math.min(width/460,height/320)
            c.translate((width-460*s)/2,(height-320*s)/2); c.scale(s,s)
            // Desktop, drawn in the app: no theme icons or bitmap dependency.
            let grad=c.createLinearGradient(80,50,380,240)
            grad.addColorStop(0,"#274d3b");grad.addColorStop(1,"#101d19")
            c.fillStyle="#0b1510";c.fillRect(65,44,291,193)
            c.strokeStyle="#6d9b81";c.lineWidth=2;c.strokeRect(65,44,291,193)
            c.fillStyle=grad;c.fillRect(73,52,275,174)
            c.save();c.beginPath();c.rect(73,52,275,174);c.clip()
            c.fillStyle="#193328";c.beginPath();c.arc(300,90,100,0,Math.PI*2);c.fill()
            c.fillStyle="#274c37";c.beginPath();c.arc(335,173,90,0,Math.PI*2);c.fill()
            c.restore()
            c.strokeStyle="#b6f3cf";c.lineWidth=2.5;c.beginPath();c.moveTo(106,138);c.lineTo(174,138);c.lineTo(165,128);c.moveTo(174,138);c.lineTo(165,148);c.stroke()
            c.fillStyle="#b6f3cf";c.font="bold 20px sans-serif";c.fillText("hello, Mac.",106,113)
            c.fillStyle="#527360";c.fillRect(94,203,13,7);c.fillRect(114,203,13,7);c.fillRect(134,203,13,7)
            c.fillStyle="#5d826b";c.fillRect(197,237,26,31);c.fillRect(168,268,83,5)
            let tower=c.createLinearGradient(361,93,410,250)
            tower.addColorStop(0,"#c5d1c7");tower.addColorStop(1,"#7f9187")
            c.fillStyle=tower;c.fillRect(365,107,50,151)
            c.fillStyle="#5b7565";for(let x=372;x<408;x+=8)for(let y=117;y<228;y+=8){c.beginPath();c.arc(x,y,1.7,0,Math.PI*2);c.fill()}
            c.fillStyle="#d7ffe5";c.beginPath();c.arc(390,246,2.5,0,Math.PI*2);c.fill()
            c.strokeStyle="#3c5948";c.lineWidth=1;c.beginPath();c.moveTo(40,279);c.lineTo(438,279);c.stroke()
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
}
