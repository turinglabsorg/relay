import QtQuick

Canvas {
    id: icon
    property string name: "screen"
    property color ink: "#a9f0c7"
    implicitWidth: 24
    implicitHeight: 24
    onNameChanged: requestPaint()
    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        let c = getContext("2d")
        c.reset(); c.scale(width / 24, height / 24)
        c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 1.65
        c.lineCap = "round"; c.lineJoin = "round"
        function line(points) { c.beginPath(); c.moveTo(points[0], points[1]); for(let i=2;i<points.length;i+=2)c.lineTo(points[i],points[i+1]); c.stroke() }
        function box(x,y,w,h) { c.strokeRect(x,y,w,h) }
        if (name === "screen") { box(3,4,18,12); line([8,21,16,21]); line([12,16,12,21]) }
        else if (name === "arrow") { line([4,12,20,12]); line([14,6,20,12,14,18]) }
        else if (name === "back") { line([20,12,4,12]); line([10,6,4,12,10,18]) }
        else if (name === "grid") { box(3,3,7,7); box(14,3,7,7); box(3,14,7,7); box(14,14,7,7) }
        else if (name === "settings") {
            c.beginPath(); c.arc(12,12,3.5,0,Math.PI*2); c.stroke()
            for(let i=0;i<8;i++){ let a=i*Math.PI/4; line([12+Math.cos(a)*7,12+Math.sin(a)*7,12+Math.cos(a)*10,12+Math.sin(a)*10]) }
        }
        else if (name === "lock") { box(5,10,14,11); c.beginPath(); c.arc(12,10,4,Math.PI,Math.PI*2); c.stroke(); line([12,14,12,17]) }
        else if (name === "refresh") { c.beginPath(); c.arc(12,12,8,.5,5.5); c.stroke(); line([16,3,19,6,19,1]) }
        else if (name === "expand") { line([9,3,3,3,3,9]); line([15,3,21,3,21,9]); line([21,15,21,21,15,21]); line([9,21,3,21,3,15]) }
        else if (name === "camera") { line([3,8,7,8,9,5,15,5,17,8,21,8,21,20,3,20,3,8]); c.beginPath(); c.arc(12,13.5,3.5,0,Math.PI*2); c.stroke() }
        else if (name === "power") { line([12,2,12,11]); c.beginPath(); c.arc(12,13,8,-1,.0+Math.PI*1.3); c.stroke() }
        else if (name === "help") { c.beginPath(); c.arc(12,12,9,0,Math.PI*2); c.stroke(); c.beginPath(); c.arc(12,9,3,Math.PI,Math.PI*2.8); c.stroke(); line([12,12,12,14]); line([12,17,12,17.2]) }
        else if (name === "sun") { c.beginPath(); c.arc(12,12,4,0,Math.PI*2); c.stroke(); for(let i=0;i<8;i++){let a=i*Math.PI/4;line([12+Math.cos(a)*7,12+Math.sin(a)*7,12+Math.cos(a)*10,12+Math.sin(a)*10])} }
        else if (name === "check") { line([5,12,10,17,20,6]) }
        else if (name === "close") { line([5,5,19,19]); line([19,5,5,19]) }
        else if (name === "keyboard") { box(2,5,20,14); for(let x=5;x<21;x+=4){line([x,9,x,9.2]);line([x,12,x,12.2])}line([7,16,17,16]) }
        else if (name === "dots") { for(let x=4;x<23;x+=8){c.beginPath();c.arc(x,12,1.5,0,Math.PI*2);c.fill()} }
    }
}
