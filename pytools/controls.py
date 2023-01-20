from abc import abstractmethod
from typing import List, Union, Tuple
from datasources import *

from PyQt5 import QtWidgets, QtSvg
from PyQt5.QtCore import Qt, QRectF
from PyQt5.QtGui import QPainter, QPen
from PyQt5.QtWidgets import QTableWidget, QTableWidgetItem, QHeaderView, QLabel

from pyqtgraph.Qt import QtGui
import pyqtgraph as pg
from common import ColorInterp


class MyQtWidget(QtWidgets.QWidget):
    def __init__(self, layout_vertical=True, **kwargs):
        super().__init__(**kwargs)
        self.layout = QtWidgets.QBoxLayout(QtWidgets.QBoxLayout.TopToBottom if layout_vertical else QtWidgets.QBoxLayout.LeftToRight)
        self.setLayout(self.layout)

    @staticmethod
    def mk_svg(file, z_val=0):
        item = QtSvg.QGraphicsSvgItem(file)
        item.setCacheMode(QtWidgets.QGraphicsItem.CacheMode.NoCache)
        item.setZValue(z_val)
        return item

    @staticmethod
    def draw_item(canvas, item):
        canvas.save()

        width, height = canvas.device().width(), canvas.device().height()
        canvas.translate(width / 2+item.x(), height / 2+item.y())
        canvas.rotate(item.rotation())

        # item.x()
        item.renderer().render(canvas, QRectF(-width / 2-item.x(), -height / 2-item.y(), 180, 180))
        canvas.restore()

class ewBasic:
    def __init__(self):
        self.data_sources: List[DataSourceBasic] = []
        self.nested_widgets: List[ewBasic] = []

    def set_data_sources(self, data_sources: List[DataSourceBasic]):
        self.data_sources = data_sources

    def add_nested(self, w):
        self.nested_widgets.append(w)

    @abstractmethod
    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        pass

    def redraw(self):
        for w in self.nested_widgets:
            w.redraw()

        vals = []
        for ds in self.data_sources:
            vals.append(ds.read())

        self.radraw_handler(vals)


class ewGroup(MyQtWidget, ewBasic):
    def __init__(self, widgets: List[MyQtWidget]):
        MyQtWidget.__init__(self, layout_vertical=False)
        ewBasic.__init__(self)

        self.group_widgets = widgets

        for w in self.group_widgets:
            self.nested_widgets.append(w)
            self.layout.addWidget(w)


class ewTable(MyQtWidget, ewBasic):
    def __init__(self, *, caption='', data_sources: List[DataSourceBasic], **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        self.set_data_sources(data_sources)

        self.table = QTableWidget()

        self.table.setRowCount(len(data_sources))
        self.table.setColumnCount(2)
        # self.table.setColumnCount(3)

        active_color = [0, 100, 0, 255]
        idle_color = self.table.palette().color(QtGui.QPalette.Base).getRgb()

        self.color_blenders: List[ColorInterp] = []
        for i in range(0, len(self.data_sources)):
            self.table.setItem(i, 0, QTableWidgetItem(data_sources[i].name))
            self.table.setItem(i, 1, QTableWidgetItem(''))
            self.color_blenders.append(ColorInterp(idle_color, active_color, 0.01))

        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.horizontalHeader().setVisible(False)
        # self.table.verticalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.verticalHeader().setVisible(False)

        height = 0
        # height = self.table.horizontalScrollBar().height() + self.table.horizontalHeader().height()
        for i in range(self.table.rowCount()):
            height += self.table.verticalHeader().sectionSize(i)

        self.table.setMinimumHeight(height)

        if caption:
            self.layout.addWidget(QLabel(caption))

        self.layout.addWidget(self.table)

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        for i in range(0, len(self.data_sources)):
            value_item = self.table.item(i, 1)
            color_item = self.table.item(i, 1)
            prev_val = value_item.text()
            new_val = str(vals[i])
            if prev_val == new_val:
                self.color_blenders[i].shift_to_left()
                color_item.setBackground(self.color_blenders[i].color_get())
            else:
                color_item.setBackground(self.color_blenders[i].set_right())

            value_item.setText(new_val)


class ewChart(MyQtWidget, ewBasic):
    colors_series = [
        (255, 0, 0),
        (0, 200, 0),
        (0, 0, 255),
        (0, 0, 0),
    ]

    class Plot:
        def __init__(self, *, label, graph, color=None, window=600, **kwargs):
            self.label = label
            self.x = list(range(window))
            self.y = [0.0] * window

            self.parent_graph = graph

            if not color:
                color = ewChart.colors_series[0]

            pen = pg.mkPen(color=color, width=4)
            self.data_line = graph.plot(self.x, self.y, pen=pen)

            self.no_data_message_is_there = False
            self.no_data_message = pg.TextItem('', anchor=(0.5, 0.5), color=color)
            self.no_data_message.setPos(100, 100)
            # self.no_data_message.setZValue(50)
            self.no_data_message.setFont(QtGui.QFont("Arial", 14))

        def update(self, val):
            if isinstance(val, NoDataStub):
                if not self.no_data_message_is_there:
                    self.no_data_message.setText(val.err_msg)
                    # self.parent_graph.addItem(self.no_data_message)
                    self.no_data_message_is_there = True
                val = 0.0
            else:
                if self.no_data_message_is_there:
                    # self.parent_graph.removeItem(self.no_data_message)
                    self.no_data_message_is_there = False

            self.x = self.x[1:]
            self.x.append(self.x[-1] + 1)

            self.y = self.y[1:]
            self.y.append(val)

            self.data_line.setData(self.x, self.y)

    def __init__(self, data_sources: List[DataSourceBasic], data_range=None, **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        self.set_data_sources(data_sources)

        self.graph = pg.PlotWidget(**kwargs)

        if data_range:
            self.graph.setYRange(data_range[0], data_range[1])

        self.layout.addWidget(self.graph)

        # window = 600
        # self.graph.setBackground(QtGui.QColor('white'))

        self.plots: List[ewChart.Plot] = []

        color_i = 0
        for ds in data_sources:
            self.plots.append(ewChart.Plot(label=ds.name, graph=self.graph, color=self.colors_series[color_i]))
            if color_i < len(self.colors_series):
                color_i += 1

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        for i in range(0, len(self.plots)):
            self.plots[i].update(vals[i])


class ewCursor(MyQtWidget, ewBasic):
    colors_series=[
        (255, 0, 0),
        (0, 200, 0),
        (0, 0, 255),
        (0, 0, 0),
    ]

    class Cursor:
        def __init__(self, *, parent, color=None, tail_len=100, **kwargs):
            self.parent = parent
            self.data = [(0.0, 0.0)]
            self.tail_len = tail_len
            if not color:
                color = ewChart.colors_series[0]

            self.color = color

        margin = 10

        def rel_x(self, x_val):
            return self.margin + (x_val - self.parent.data_range[0][0]) * (self.parent.width() - 2*self.margin) / self.parent.x_range_mod

        def rel_y(self, y_val):
            return self.margin + (y_val - self.parent.data_range[1][0]) * (self.parent.height() - 2*self.margin) / self.parent.y_range_mod

        def update_data(self, val: tuple):
            if isinstance(val[0], NoDataStub) or isinstance(val[1], NoDataStub):
                return

            if self.data[-1] != val:
                self.data.append(val)

            if len(self.data) > self.tail_len:
                self.data = self.data[1:]

        def redraw(self, painter):
            b = len(self.data)
            for i in range(0, b):
                if i == b-1:
                    alpha = 255
                    thickness = 2
                    d = 8
                else:
                    alpha = int(i * 100.0 / b)
                    thickness = 1
                    d = 2

                painter.setPen(
                    QPen(QtGui.QColor(self.color[0], self.color[1], self.color[2], alpha), thickness, Qt.SolidLine))

                x = self.rel_x(self.data[i][0])
                y = self.rel_y(self.data[i][1])
                d_2 = d/2
                painter.drawEllipse(int(x - d_2), int(y - d_2), int(d), int(d))

    def __init__(self, data_sources: List[Tuple[DataSourceBasic, DataSourceBasic]], data_range=((-1.0, 1.0), (-1.0, 1.0)), *args, **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        data_sources_list = [ elem for t in data_sources for elem in t]
        self.set_data_sources(data_sources_list)

        self.setFixedSize(180, 180)
        # self.setMinimumHeight(80)
        # self.setMinimumWidth(80)

        self.layout.addWidget(self)

        self.data_range = data_range
        self.x_range_mod = data_range[0][1] - data_range[0][0]
        self.y_range_mod = data_range[1][1] - data_range[1][0]

        self.cursors: List[ewChart.Cursor] = []

        color_i = 0
        for ds in data_sources:
            self.cursors.append(ewCursor.Cursor(parent=self, color=self.colors_series[color_i]))
            if color_i < len(self.colors_series):
                color_i += 1

    def paintEvent(self, QPaintEvent):
        canvas = QPainter(self)

        for c in self.cursors:
            c.redraw(canvas)

        canvas.end()

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        for i in range(0, len(self.cursors)):
            self.cursors[i].update_data((vals[i*2], vals[i*2 + 1]))

        self.repaint()


class ewPaintSample(MyQtWidget, ewBasic):
    # SAMPLE_TODO copy and paste this class , rename properly

    def __init__(self, data_sources: List[DataSourceBasic], **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        # SAMPLE_TODO pass list of data to
        self.set_data_sources(data_sources)

        # SAMPLE_TODO define widget size policy
        self.setFixedSize(180, 180)
        # self.setMinimumHeight(80)
        # self.setMinimumWidth(80)

        self.layout.addWidget(self)

    def paintEvent(self, QPaintEvent):
        canvas = QPainter(self)

        # SAMPLE_TODO drawing code
        canvas.end()

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):

        # SAMPLE_TODO pass upcoming data to widget's state
        self.repaint()


class ewHeadingIndicator(MyQtWidget, ewBasic):
    # SAMPLE_TODO copy and paste this class , rename properly

    def __init__(self, data_sources: List[DataSourceBasic], **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        # SAMPLE_TODO pass list of data to
        self.set_data_sources(data_sources)

        # SAMPLE_TODO define widget size policy
        self.setFixedSize(180, 180)
        # self.setMinimumHeight(80)
        # self.setMinimumWidth(80)

        self.svgBack = self.mk_svg("images/hi/hi_face.svg")
        self.svgFace = self.mk_svg("images/hi/hi_case.svg")
        self._heading = 0.0
        self.layout.addWidget(self)

    def paintEvent(self, event):
        canvas = QPainter(self)
        self.svgFace.setRotation(-self._heading)
        self.draw_item(canvas, self.svgBack)
        self.draw_item(canvas, self.svgFace)
        canvas.end()

    def set_heading(self, h):
        self._heading = h

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        self.set_heading(vals[0])
        self.repaint()


class ewAttitudeIndicator(MyQtWidget, ewBasic):
    def __init__(self, data_sources: List[DataSourceBasic], **kwargs):
        MyQtWidget.__init__(self, **kwargs)
        ewBasic.__init__(self)

        # SAMPLE_TODO pass list of data to
        self.set_data_sources(data_sources)

        # SAMPLE_TODO define widget size policy
        self.setFixedSize(180, 180)
        # self.setMinimumHeight(80)
        # self.setMinimumWidth(80)

        self.svgBack = self.mk_svg("images/ai/ai_back.svg", -30)
        self.svgFace = self.mk_svg("images/ai/ai_face.svg")
        self.svgRing = self.mk_svg("images/ai/ai_ring.svg")
        self.svgCase = self.mk_svg("images/ai/ai_case.svg")

        self._roll = 0.0
        self._pitch = 0.0
        self._faceDeltaX_old = 0.0
        self._faceDeltaY_old = 0.0

        self.reset()

        self.layout.addWidget(self)

    def set_roll(self, roll):
        self._roll = roll

        if self._roll < -180.0:
            self._roll = -180.0

        if self._roll > 180.0:
            self._roll = 180.0

    def set_pitch(self, pitch):
        self._pitch = pitch

        if self._pitch < -25.0:
            self._pitch = -25.0

        if self._pitch > 25.0:
            self._pitch = 25.0

    def reset(self):
        self._roll = 0.0
        self._pitch = 0.0
        self._faceDeltaX_old = 0.0
        self._faceDeltaY_old = 0.0

    def paintEvent(self, event):
        canvas = QPainter(self)

        _width = canvas.device().width()
        _height = canvas.device().height()

        _br = self.svgFace.boundingRect()
        _originalWidth = _br.width()
        _originalHeight = _br.height()

        _scaleX = _width / _originalWidth
        _scaleY = _height / _originalHeight

        roll_rad = math.pi * self._roll / 180.0
        delta = 1.7 * self._pitch

        _faceDeltaX_new = _scaleX * delta * math.sin(roll_rad)
        _faceDeltaY_new = _scaleY * delta * math.cos(roll_rad)

        offs_x = _faceDeltaX_new - self._faceDeltaX_old
        offs_y = _faceDeltaY_new - self._faceDeltaY_old

        self.svgBack.setRotation(-self._roll)
        self.svgFace.setRotation(-self._roll)
        self.svgRing.setRotation(-self._roll)
        self.svgFace.moveBy(offs_x, offs_y)

        self.draw_item(canvas, self.svgBack)
        self.draw_item(canvas, self.svgFace)
        self.draw_item(canvas, self.svgRing)
        self.draw_item(canvas, self.svgCase)

        self._faceDeltaX_old = _faceDeltaX_new
        self._faceDeltaY_old = _faceDeltaY_new

        canvas.end()

    def radraw_handler(self, vals: List[Union[float, int, str, NoDataStub]]):
        self.set_roll(vals[0])
        self.set_pitch(vals[1])
        self.repaint()

