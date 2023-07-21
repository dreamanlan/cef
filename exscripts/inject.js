var div = document.createElement('div');
div.style.left = '0px';
div.style.right = '0px';
div.style.width = '800px';
div.style.height = '360px';
div.style.background = 'lightblue';
div.style.position = 'absolute';
div.style.zIndex = 999999;
//document.body.appendChild(div);
document.body.insertBefore(div, document.body.firstChild);
var drag = false;
var currentX;
var currentY;
// 鼠标按下事件
div.addEventListener('mousedown', function(event) {
  drag = true;
  currentX = event.clientX - div.offsetLeft;
  currentY = event.clientY - div.offsetTop;
});
// 鼠标移动事件
document.addEventListener('mousemove', function(event) {
  if (drag === true) {
    div.style.left = event.clientX - currentX + 'px';
    div.style.top = event.clientY - currentY + 'px';
  }
});
// 鼠标松开事件
document.addEventListener('mouseup', function() {
  drag = false;
});

//嵌入内容
var div1 = document.createElement("DIV");
div1.style.width="100%";
div1.style.height="300px";
var div2 = document.createElement("DIV");
div2.style.width="100%";
div2.style.height="40px";
div2.style.display="flex";
div2.style.alignItems="center";
div2.style.justifyContent="center";
var hr = document.createElement("HR");
var txtArea = document.createElement("TEXTAREA");
txtArea.style.width="100%";
txtArea.style.height="100%";
var btn = document.createElement("BUTTON");
var t = document.createTextNode("Run");
btn.appendChild(t);
btn.style.width="120px";
btn.style.height="100%";
div1.appendChild(txtArea);
div2.appendChild(btn);
btn.onclick=function(){
  eval(txtArea.value);
};

div.appendChild(div1);
div.appendChild(hr);
div.appendChild(div2);